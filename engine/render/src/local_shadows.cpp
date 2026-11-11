#include "local_shadows.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace giant::render {

namespace {

constexpr u64 k_forget_frames = 600; // an entry unused this long gives its tiles back
constexpr float k_guard = 2.0f;      // texels of guard band round a cube face

bool sphere_touches_box(const DVec3& c, double r, const DVec3& lo, const DVec3& hi) {
    const double dx = std::max({lo.x - c.x, 0.0, c.x - hi.x});
    const double dy = std::max({lo.y - c.y, 0.0, c.y - hi.y});
    const double dz = std::max({lo.z - c.z, 0.0, c.z - hi.z});
    return dx * dx + dy * dy + dz * dz <= r * r;
}

Vec3 unit_or(const Vec3& v, const Vec3& fallback) {
    const float len = length(v);
    return len > 1e-6f && std::isfinite(len) ? v * (1.0f / len) : fallback;
}

float clamped_near(const LocalLight& l, float range) {
    const float n = std::isfinite(l.shadow_near) ? l.shadow_near : 0.1f;
    return std::clamp(n, 0.01f, std::max(range * 0.5f, 0.01f));
}

} // namespace

void LocalShadows::face_axes(LightKind kind, const Vec3& direction, u32 face, Vec3& forward, Vec3& right) {
    if (kind == LightKind::spot) {
        forward = unit_or(direction, {0.0f, -1.0f, 0.0f});
        // Across the axis: the world axis least along it, made square to it.
        const Vec3 a = std::fabs(forward.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
        right = unit_or(a - forward * dot(a, forward), {0.0f, 0.0f, 1.0f});
        return;
    }
    const float s = (face & 1u) == 0u ? 1.0f : -1.0f;
    switch (face / 2) {
    case 0:
        forward = {s, 0.0f, 0.0f};
        right = {0.0f, 0.0f, 1.0f};
        break;
    case 1:
        forward = {0.0f, s, 0.0f};
        right = {1.0f, 0.0f, 0.0f};
        break;
    default:
        forward = {0.0f, 0.0f, s};
        right = {1.0f, 0.0f, 0.0f};
        break;
    }
}

float LocalShadows::tan_half(LightKind kind, float outer_angle) {
    if (kind == LightKind::spot) {
        const float a = std::isfinite(outer_angle) ? std::clamp(outer_angle, 0.001f, 1.553f) : 1.553f;
        return std::tan(a) * (1.0f + 2.0f * k_guard / static_cast<float>(k_local_shadow_size));
    }
    // A little over 45 degrees: k_guard texels each side for filtering.
    return 1.0f + 2.0f * k_guard / static_cast<float>(k_local_shadow_size);
}

Mat4 LocalShadows::face_matrix(const Face& f, const DVec3& camera) {
    const Vec3 l = to_camera_relative(f.position, camera);
    const Vec3 up = cross(f.right, f.forward);
    const float inv = 1.0f / f.tan_half;
    Mat4 m;
    const auto row = [&m](int r, const Vec3& v, float w) {
        m.at(r, 0) = v.x;
        m.at(r, 1) = v.y;
        m.at(r, 2) = v.z;
        m.at(r, 3) = w;
    };
    row(0, f.right * inv, -dot(f.right, l) * inv);
    row(1, up * inv, -dot(up, l) * inv);
    row(2, Vec3{}, f.near);
    row(3, f.forward, -dot(f.forward, l));
    return m;
}

void LocalShadows::face_planes(const Face& f, const DVec3& camera, float out[20]) {
    const Vec3 l = to_camera_relative(f.position, camera);
    const Vec3 up = cross(f.right, f.forward);
    const auto plane = [&](u32 i, const Vec3& n_raw, float extra) {
        const Vec3 n = unit_or(n_raw, f.forward);
        out[i * 4 + 0] = n.x;
        out[i * 4 + 1] = n.y;
        out[i * 4 + 2] = n.z;
        out[i * 4 + 3] = -dot(n, l) - extra;
    };
    plane(0, f.forward * f.tan_half + f.right, 0.0f); // x >= -1
    plane(1, f.forward * f.tan_half - f.right, 0.0f); // x <= 1
    plane(2, f.forward * f.tan_half + up, 0.0f);
    plane(3, f.forward * f.tan_half - up, 0.0f);
    plane(4, f.forward, f.near);
}

u32 LocalShadows::tiles_used() const {
    u32 n = 0;
    for (const u32 o : m_owner) n += o != k_free ? 1u : 0u;
    return n;
}

bool LocalShadows::allocate(u32 count, u32& first) {
    for (u32 t = 0; t + count <= m_tiles; ++t) {
        u32 k = 0;
        while (k < count && m_owner[t + k] == k_free) ++k;
        if (k == count) {
            first = t;
            return true;
        }
        t += k; // past the taken tile
    }
    return false;
}

void LocalShadows::release(u32 id) {
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    for (u32 k = 0; k < it->second.count; ++k) m_owner[it->second.first + k] = k_free;
    m_entries.erase(it);
}

void LocalShadows::undo_draws() {
    for (const Face& f : m_draws) {
        const u32 id = f.tile < m_owner.size() ? m_owner[f.tile] : k_free;
        const auto it = m_entries.find(id);
        if (it == m_entries.end()) continue;
        const u32 face = f.tile - it->second.first;
        it->second.dirty |= 1u << face;
        it->second.drawn &= ~(1u << face); // what the tile holds is unknown
    }
    m_draws.clear();
}

void LocalShadows::invalidate_all() {
    for (auto& [id, e] : m_entries) e.dirty = (1u << e.count) - 1u;
}

void LocalShadows::plan(std::span<const ShadowCandidate> candidates, const DVec3& camera,
                        std::span<const std::pair<DVec3, DVec3>> changed, bool all, u32 budget) {
    (void)camera;
    ++m_frame;
    m_draws.clear();
    m_shadowed.clear();
    m_waiting = 0;
    // Entries long unused give their tiles back.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->second.used + k_forget_frames < m_frame) {
            for (u32 k = 0; k < it->second.count; ++k) m_owner[it->second.first + k] = k_free;
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // Casters near a light came, went or moved: drawn again (the old depths
    // stand in meanwhile), whether it is wanted this frame or not.
    for (auto& [id, e] : m_entries) {
        const u32 every = (1u << e.count) - 1u;
        if (all) {
            e.dirty = every;
            continue;
        }
        for (u32 i = 0; i < changed.size() && e.dirty != every; ++i) {
            if (sphere_touches_box(e.position, e.range, changed[i].first, changed[i].second)) e.dirty = every;
        }
    }
    struct Taken {
        u32 light;
        u32 id;
        LocalLight params;
    };
    std::vector<Taken> taken;
    for (const ShadowCandidate& c : candidates) {
        if (taken.size() >= k_max_shadowed_lights) break;
        if (c.source == nullptr || c.source->shadow_id == 0) continue;
        const LocalLight& l = *c.source;
        const u32 id = l.shadow_id;
        const auto twice = std::find_if(taken.begin(), taken.end(), [id](const Taken& t) { return t.id == id; });
        if (twice != taken.end()) continue; // one shadow per id (the nearest light with it)
        const u32 count = l.kind == LightKind::spot ? 1u : 6u;
        const float range = std::isfinite(l.range) ? std::clamp(l.range, 0.01f, 1000.0f) : 0.01f;
        const Vec3 direction = l.kind == LightKind::spot ? unit_or(l.direction, {0.0f, -1.0f, 0.0f}) : Vec3{};
        const float outer = l.kind == LightKind::spot ? l.outer_angle : 0.0f;
        const float near = clamped_near(l, range);
        auto it = m_entries.find(id);
        if (it != m_entries.end() && it->second.count != count) {
            release(id);
            it = m_entries.end();
        }
        if (it == m_entries.end()) {
            u32 first = 0;
            bool ok = allocate(count, first);
            // Out of tiles: those of lights not wanted this frame, oldest first.
            while (!ok) {
                u32 oldest = 0;
                u64 when = ~u64{0};
                for (const auto& [other, e] : m_entries) {
                    if (e.used < m_frame && e.used < when) {
                        when = e.used;
                        oldest = other;
                    }
                }
                if (when == ~u64{0}) break;
                release(oldest);
                ok = allocate(count, first);
            }
            if (!ok) continue; // no room: unshadowed
            Entry e;
            e.first = first;
            e.count = count;
            e.dirty = (1u << count) - 1u;
            for (u32 k = 0; k < count; ++k) m_owner[first + k] = id;
            it = m_entries.emplace(id, e).first;
        }
        Entry& e = it->second;
        e.used = m_frame;
        // Changed: drawn again, unshadowed until then (old depths would put
        // shadows in the wrong places). Moved: drawn again, the faces drawn
        // from near enough standing in meanwhile.
        if (e.kind != l.kind || e.range != range || e.outer != outer || e.near != near) {
            e.kind = l.kind;
            e.range = range;
            e.outer = outer;
            e.near = near;
            e.drawn = 0;
            e.dirty = (1u << count) - 1u;
        }
        if (!(e.position == l.position) || !(e.direction == direction)) {
            e.position = l.position;
            e.direction = direction;
            e.dirty = (1u << count) - 1u;
            const double tolerance = std::max(0.01, 0.01 * double(range));
            Vec3 forward;
            Vec3 right;
            face_axes(e.kind, direction, 0, forward, right);
            for (u32 f = 0; f < count; ++f) {
                const DVec3 d = e.face_position[f] - l.position;
                // (A spot's right axis too: it can jump as the axis turns.)
                if (d.x * d.x + d.y * d.y + d.z * d.z > tolerance * tolerance ||
                    (l.kind == LightKind::spot &&
                     (dot(e.face_direction[f], direction) < 0.9999f || dot(e.face_right, right) < 0.9999f))) {
                    e.drawn &= ~(1u << f);
                }
            }
        }
        taken.push_back({c.light, id, l});
    }
    // Faces to draw: those with nothing drawn first (nearest lights first),
    // then those drawn longest ago, so lights changing every frame share
    // the budget and every face of theirs comes round.
    struct Want {
        u32 order; // in `taken`
        u32 face;
        u32 stale;  // 0: nothing drawn there, 1: old depths stand in
        u64 frame;  // when last drawn
    };
    std::vector<Want> wants;
    for (u32 i = 0; i < taken.size(); ++i) {
        const Entry& e = m_entries.at(taken[i].id);
        for (u32 face = 0; face < e.count; ++face) {
            if ((e.dirty & (1u << face)) == 0u) continue;
            wants.push_back({i, face, (e.drawn >> face) & 1u, e.face_frame[face]});
        }
    }
    std::stable_sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) {
        if (a.stale != b.stale) return a.stale < b.stale;
        if (a.stale == 1u && a.frame != b.frame) return a.frame < b.frame;
        return a.order < b.order;
    });
    if (wants.size() > budget) wants.resize(budget);
    for (const Want& w : wants) {
        Entry& e = m_entries.at(taken[w.order].id);
        Face f;
        f.tile = e.first + w.face;
        f.position = e.position;
        face_axes(e.kind, e.direction, w.face, f.forward, f.right);
        f.tan_half = tan_half(e.kind, e.outer);
        f.near = e.near;
        f.range = e.range;
        m_draws.push_back(f);
        e.face_position[w.face] = e.position;
        e.face_direction[w.face] = e.direction;
        if (w.face == 0) e.face_right = f.right;
        e.face_frame[w.face] = m_frame;
        e.dirty &= ~(1u << w.face);
        e.drawn |= 1u << w.face;
    }
    for (const Taken& t : taken) {
        Entry& e = m_entries.at(t.id);
        m_waiting += static_cast<u32>(std::popcount(e.dirty));
        if (e.drawn == 0) continue; // nothing to shade with yet
        Shadowed s;
        s.light = t.light;
        s.record.near = e.near;
        s.record.tile = e.first;
        s.record.drawn = e.drawn;
        s.record.tan_half = tan_half(e.kind, e.outer);
        // (The right axis its face was drawn with.)
        s.record.right[0] = e.face_right.x;
        s.record.right[1] = e.face_right.y;
        s.record.right[2] = e.face_right.z;
        m_shadowed.push_back(s);
    }
}

} // namespace giant::render
