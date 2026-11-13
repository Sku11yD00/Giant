#include "wet_things.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

constexpr float k_turned = 0.4f;    // the up side's up less than this: turned over, wet all over
constexpr float k_margin = 0.02f;   // metres round a body's bounds (and below it)
constexpr float k_character_margin = 0.1f; // round a character's capsule (its arms, a stride)
constexpr float k_feet_gap = 0.03f; // a character's box starts this far over its feet (not the ground)
constexpr float k_keep_line = 0.9f;  // back in this wet or wetter (out 9 s or less): the line kept
constexpr float k_waterline = 0.08f; // metres the line fades over

u64 key_of(const sim::InWater& w) {
    return w.body != sim::k_no_body ? u64{w.body} : (u64{1} << 32) | u64{w.character};
}

// A body's six sides (unit, in its frame), and the turn taking a decal's
// +Y to each (its x and z to two others).
Vec3 side(u8 axis) {
    const float s = (axis & 1u) != 0 ? -1.0f : 1.0f;
    switch (axis >> 1) {
    case 0: return {s, 0.0f, 0.0f};
    case 1: return {0.0f, s, 0.0f};
    default: return {0.0f, 0.0f, s};
    }
}

Quat turn_to(u8 axis) {
    switch (axis) {
    case 0: return quat_from_axis_angle({0.0f, 0.0f, 1.0f}, -0.5f * k_pi);
    case 1: return quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 0.5f * k_pi);
    case 3: return quat_from_axis_angle({1.0f, 0.0f, 0.0f}, k_pi);
    case 4: return quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 0.5f * k_pi);
    case 5: return quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.5f * k_pi);
    default: return Quat{};
    }
}

// The bounds' span along `d` (a side: plus or minus one of the body's axes).
void span_along(const sim::LocalBounds& b, const Vec3& d, float& lo, float& hi) {
    const float a = dot(d, b.min), c = dot(d, b.max);
    lo = std::min(a, c);
    hi = std::max(a, c);
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

} // namespace

usize WetThings::in_water() const {
    usize n = 0;
    for (const auto& [key, w] : m_things) n += w.in_water ? 1 : 0;
    return n;
}

void WetThings::soak(const sim::PhysicsWorld& physics, const sim::InWater& w) {
    const u64 key = key_of(w);
    auto it = m_things.find(key);
    if (it == m_things.end()) {
        if (m_things.size() >= k_most) {
            // Room made by the driest out of the water (the lowest key of
            // those as dry); none: not remembered (nor looked for again
            // this call).
            if (m_full) return;
            auto driest = m_things.end();
            for (auto j = m_things.begin(); j != m_things.end(); ++j) {
                if (j->second.in_water) continue;
                if (driest == m_things.end() || j->second.amount < driest->second.amount ||
                    (j->second.amount == driest->second.amount && j->first < driest->first)) {
                    driest = j;
                }
            }
            if (driest == m_things.end()) {
                m_full = true;
                return;
            }
            m_things.erase(driest);
        }
        Wet fresh;
        fresh.body = w.body;
        fresh.character = w.character;
        it = m_things.emplace(key, fresh).first;
    }
    Wet& wet = it->second;
    const bool going_in = !wet.in_water; // (new, or drying and back in)
    // Back in after drying a while (not just bobbing out a moment): wet
    // from the new waterline, the old one dried.
    if (going_in && wet.amount < k_keep_line) wet.line = 0.0f;
    wet.in_water = true;
    wet.amount = 1.0f;
    wet.seen = m_calls;
    if (w.character != sim::k_no_character) {
        const std::optional<sim::CharacterState> s = physics.character_state(w.character);
        const std::optional<sim::CharacterDesc> d = physics.character_desc(w.character);
        if (!s || !d || !(d->height > 0.0f)) return;
        const double up = (w.level - s->position.y) / static_cast<double>(d->height);
        if (std::isfinite(up)) wet.line = std::max(wet.line, static_cast<float>(std::clamp(up, 0.0, 1.0)));
        return;
    }
    const std::optional<sim::BodyState> s = physics.state(w.body);
    const std::optional<sim::LocalBounds> b = physics.local_bounds(w.body);
    if (!s || !b) return;
    const Quat r = normalize(s->rotation);
    if (going_in) {
        // The side most up as it goes in (back in another way up: wet
        // along that side from now, the old line forgotten).
        float most = -2.0f;
        u8 axis = wet.axis;
        for (u8 a = 0; a < 6; ++a) {
            const float y = rotate(r, side(a)).y;
            if (y > most) {
                most = y;
                axis = a;
            }
        }
        if (axis != wet.axis) wet.line = 0.0f;
        wet.axis = axis;
    }
    const Vec3 d = side(wet.axis);
    const float u = rotate(r, d).y;
    if (!(u >= k_turned)) {
        wet.line = 1.0f; // turned over in the water: wet all over
        return;
    }
    float lo = 0.0f, hi = 0.0f;
    span_along(*b, d, lo, hi);
    if (!(hi - lo > 1e-4f)) {
        wet.line = 1.0f;
        return;
    }
    // Where the surface crosses the body along its up side (the bounds'
    // middle line through it, leaning as it leans).
    const Vec3 middle = (b->min + b->max) * 0.5f;
    const double y0 = s->position.y + static_cast<double>(rotate(r, middle).y);
    const double t = static_cast<double>(dot(d, middle)) + (w.level - y0) / static_cast<double>(u);
    const double f = (t - static_cast<double>(lo)) / static_cast<double>(hi - lo);
    // (Listed: some of it is under, if only a corner as it leans.)
    if (std::isfinite(f)) wet.line = std::max(wet.line, static_cast<float>(std::clamp(f, 0.05, 1.0)));
}

void WetThings::after_step(const sim::PhysicsWorld& physics, double seconds) {
    if (!(seconds > 0.0)) return; // no step ran: nothing looked at
    ++m_calls;
    m_full = false;
    // Each once (the highest the water got over it in the call's steps), by key.
    m_list.assign(physics.in_water().begin(), physics.in_water().end());
    std::sort(m_list.begin(), m_list.end(), [](const sim::InWater& a, const sim::InWater& b) {
        const u64 ka = key_of(a), kb = key_of(b);
        return ka != kb ? ka < kb : a.level > b.level;
    });
    for (usize i = 0; i < m_list.size(); ++i) {
        if (i > 0 && key_of(m_list[i]) == key_of(m_list[i - 1])) continue;
        soak(physics, m_list[i]);
    }
    const float dried = static_cast<float>(seconds) / k_dry_seconds;
    for (auto it = m_things.begin(); it != m_things.end();) {
        Wet& w = it->second;
        if (w.seen != m_calls) {
            if (w.body != sim::k_no_body) {
                const std::optional<sim::BodyState> s = physics.state(w.body);
                if (!s) {
                    it = m_things.erase(it); // gone
                    continue;
                }
                if (s->active) w.in_water = false; // awake and not under: out (asleep: as it was)
            } else {
                if (!physics.contains_character(w.character)) {
                    it = m_things.erase(it);
                    continue;
                }
                w.in_water = false;
            }
        }
        if (!w.in_water) {
            w.amount -= dried;
            if (!(w.amount > 0.0f)) {
                it = m_things.erase(it); // dry
                continue;
            }
        }
        ++it;
    }
}

void WetThings::decals(const sim::PhysicsWorld& physics, std::vector<render::Decal>& out) const {
    for (const auto& [key, w] : m_things) {
        if (!(w.line > 0.0f) || !(w.amount > 0.0f)) continue;
        render::Decal d;
        d.shape = render::DecalShape::wet;
        d.opacity = std::min(w.amount, 1.0f);
        // The fade above the line (all of it wet: past the top, no fade).
        const bool all = w.line >= 1.0f;
        if (w.character != sim::k_no_character) {
            // Drawn only if it has a body to see (a walker's entity; the player has none).
            if (physics.user(physics.character_body(w.character)) == 0) continue;
            const std::optional<sim::CharacterState> s = physics.character_state(w.character);
            const std::optional<sim::CharacterDesc> c = physics.character_desc(w.character);
            if (!s || !c) continue;
            // Upright round its feet as drawn (between steps), from just
            // over them (not the ground under).
            const float top = all ? c->height + k_character_margin : w.line * c->height + 0.5f * k_waterline;
            const float height = std::max(top - k_feet_gap, k_margin);
            const float across = 2.0f * (c->radius + k_character_margin);
            const DVec3 feet = lerp(s->previous, s->position, physics.step_fraction());
            d.position = feet + DVec3{0.0, static_cast<double>(k_feet_gap + 0.5f * height), 0.0};
            d.size = {across, height, across};
            d.softness = all ? 0.0f : std::min(k_waterline / height, 0.5f);
            out.push_back(d);
            continue;
        }
        const std::optional<sim::BodyState> s = physics.state(w.body);
        const std::optional<sim::LocalBounds> b = physics.local_bounds(w.body);
        if (!s || !b) continue;
        const Quat r = normalize(s->rotation);
        const Quat a = turn_to(w.axis);
        const Vec3 dx = rotate(a, {1.0f, 0.0f, 0.0f});
        const Vec3 dy = side(w.axis);
        const Vec3 dz = rotate(a, {0.0f, 0.0f, 1.0f});
        float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f, z0 = 0.0f, z1 = 0.0f;
        span_along(*b, dx, x0, x1);
        span_along(*b, dy, y0, y1);
        span_along(*b, dz, z0, z1);
        // From a little under its bottom up to the line.
        const float height = w.line * (y1 - y0) + k_margin + (all ? k_margin : 0.5f * k_waterline);
        const Vec3 centre = dx * (0.5f * (x0 + x1)) + dy * (y0 - k_margin + 0.5f * height) + dz * (0.5f * (z0 + z1));
        if (!finite(centre) || !(height > 0.0f)) continue;
        d.position = to_world(rotate(r, centre), s->position);
        d.rotation = normalize(r * a);
        d.size = {x1 - x0 + 2.0f * k_margin, height, z1 - z0 + 2.0f * k_margin};
        d.softness = all ? 0.0f : std::min(k_waterline / height, 0.5f);
        out.push_back(d);
    }
}

} // namespace giant::editor
