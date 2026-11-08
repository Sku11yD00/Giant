#include "giant/sim/hit_shapes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace giant::sim {

namespace {

constexpr double k_inf = std::numeric_limits<double>::infinity();

double ddot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
DVec3 dvec(const Vec3& v) { return {v.x, v.y, v.z}; }

using Interval = std::optional<std::pair<double, double>>;

Interval ray_sphere(const DVec3& o, const DVec3& d, const DVec3& c, double r) {
    const DVec3 m = o - c;
    const double b = ddot(m, d);
    const double disc = b * b - (ddot(m, m) - r * r);
    if (disc < 0.0) return std::nullopt;
    const double s = std::sqrt(disc);
    return std::pair{-b - s, -b + s};
}

bool contains_word(std::string_view name, std::string_view word) {
    if (word.empty() || word.size() > name.size()) return false;
    for (usize i = 0; i + word.size() <= name.size(); ++i) {
        bool same = true;
        for (usize k = 0; k < word.size() && same; ++k) {
            same = std::tolower(static_cast<unsigned char>(name[i + k])) ==
                   std::tolower(static_cast<unsigned char>(word[k]));
        }
        if (same) return true;
    }
    return false;
}

} // namespace

u16 HitZoneMap::zone_of(std::string_view bone) const {
    for (usize i = 0; i < rules.size() && i < k_no_zone; ++i) {
        for (const std::string& w : rules[i].match) {
            if (contains_word(bone, w)) return static_cast<u16>(i);
        }
    }
    return k_no_zone;
}

HitZoneMap default_hit_zones() {
    HitZoneMap m;
    // In order: the first rule a bone's name matches names it.
    m.rules.push_back({"head", {"head", "skull", "jaw", "eye"}});
    m.rules.push_back({"neck", {"neck"}});
    m.rules.push_back({"arm", {"arm", "hand", "finger", "thumb", "index", "middle", "ring", "pinky", "wrist", "elbow"}});
    m.rules.push_back({"leg", {"leg", "thigh", "knee", "calf", "shin", "foot", "toe", "ankle"}});
    m.rules.push_back({"torso", {"spine", "chest", "hips", "pelvis", "shoulder", "clavicle", "torso", "root", "body"}});
    return m;
}

Interval ray_capsule(const DVec3& origin, const Vec3& direction, const DVec3& a, const DVec3& b, double radius) {
    if (!(radius > 0.0)) return std::nullopt;
    const DVec3 d = dvec(direction);
    // The capsule is the union of its two end balls and the cylinder
    // between them: convex, so the line goes in where it first enters any
    // of them and out where it last leaves one.
    Interval out;
    const auto take = [&out](const Interval& i) {
        if (!i || i->first > i->second) return;
        out = out ? std::pair{std::min(out->first, i->first), std::max(out->second, i->second)} : *i;
    };
    take(ray_sphere(origin, d, a, radius));
    take(ray_sphere(origin, d, b, radius));
    const DVec3 axis = b - a;
    const double length = std::sqrt(ddot(axis, axis));
    if (length > 1e-9) {
        const DVec3 u = axis * (1.0 / length);
        const DVec3 m = origin - a;
        const double du = ddot(d, u);
        const double mu = ddot(m, u);
        const DVec3 dp = d - u * du;
        const DVec3 mp = m - u * mu;
        const double aa = ddot(dp, dp);
        Interval side; // within the infinite cylinder
        if (aa < 1e-12) {
            if (ddot(mp, mp) <= radius * radius) side = std::pair{-k_inf, k_inf};
        } else {
            const double bb = ddot(mp, dp);
            const double disc = bb * bb - aa * (ddot(mp, mp) - radius * radius);
            if (disc >= 0.0) {
                const double s = std::sqrt(disc);
                side = std::pair{(-bb - s) / aa, (-bb + s) / aa};
            }
        }
        Interval slab; // between the end planes
        if (std::fabs(du) < 1e-12) {
            if (mu >= 0.0 && mu <= length) slab = std::pair{-k_inf, k_inf};
        } else {
            const double t0 = -mu / du;
            const double t1 = (length - mu) / du;
            slab = std::pair{std::min(t0, t1), std::max(t0, t1)};
        }
        if (side && slab) take(std::pair{std::max(side->first, slab->first), std::min(side->second, slab->second)});
    }
    return out;
}

void HitShapes::set(u64 owner, HitCharacter character) {
    if (character.capsules.empty()) {
        remove(owner);
        return;
    }
    Entry e;
    DVec3 sum{};
    for (const HitCapsule& c : character.capsules) sum = sum + c.a + c.b;
    e.center = sum * (1.0 / (2.0 * static_cast<double>(character.capsules.size())));
    for (const HitCapsule& c : character.capsules) {
        const DVec3 da = c.a - e.center;
        const DVec3 db = c.b - e.center;
        e.radius = std::max({e.radius, std::sqrt(ddot(da, da)) + c.radius, std::sqrt(ddot(db, db)) + c.radius});
    }
    e.character = std::move(character);
    m_characters[owner] = std::move(e);
}

void HitShapes::remove(u64 owner) { m_characters.erase(owner); }

void HitShapes::clear() { m_characters.clear(); }

const HitCharacter* HitShapes::find(u64 owner) const {
    const auto it = m_characters.find(owner);
    return it == m_characters.end() ? nullptr : &it->second.character;
}

std::optional<ShapeHit> HitShapes::raycast(const DVec3& origin, const Vec3& direction, double max_distance,
                                           u64 ignore) const {
    std::optional<ShapeHit> best;
    if (!(max_distance > 0.0)) return best;
    const DVec3 d = dvec(direction);
    for (const auto& [owner, e] : m_characters) {
        if (owner == ignore) continue;
        const Interval round = ray_sphere(origin, d, e.center, e.radius);
        if (!round || round->second < 0.0 || round->first > max_distance) continue;
        for (u32 i = 0; i < e.character.capsules.size(); ++i) {
            const HitCapsule& c = e.character.capsules[i];
            const Interval in = ray_capsule(origin, direction, c.a, c.b, c.radius);
            if (!in || in->second < 0.0 || in->first > max_distance) continue;
            const double at = std::max(in->first, 0.0);
            // Nearest; ties go the same way every run (lowest owner, then capsule).
            if (best && (at > best->distance || (at == best->distance && (owner > best->owner ||
                                                                          (owner == best->owner && i > best->capsule))))) {
                continue;
            }
            ShapeHit h;
            h.owner = owner;
            h.capsule = i;
            h.distance = at;
            h.exit = in->second;
            h.point = origin + d * at;
            // Out of the capsule: from the nearest point of its segment.
            const DVec3 axis = c.b - c.a;
            const double len2 = ddot(axis, axis);
            const double s = len2 > 0.0 ? std::clamp(ddot(h.point - c.a, axis) / len2, 0.0, 1.0) : 0.0;
            const DVec3 n = h.point - (c.a + axis * s);
            const double nl = std::sqrt(ddot(n, n));
            h.normal = nl > 1e-9 ? Vec3{static_cast<float>(n.x / nl), static_cast<float>(n.y / nl),
                                        static_cast<float>(n.z / nl)}
                                 : -direction;
            best = h;
        }
    }
    return best;
}

} // namespace giant::sim
