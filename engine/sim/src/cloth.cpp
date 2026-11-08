#include "giant/sim/cloth.h"

#include <algorithm>
#include <cmath>

namespace giant::sim {

namespace {

constexpr float k_tau = 6.28318530717958647692f;
constexpr float k_drag = 1.2f;        // a flat plate face-on
constexpr float k_skin_drag = 0.05f;  // along its face
constexpr float k_ripple = 0.25f;     // the ripples' push, against the face-on push
constexpr float k_damping = 0.01f;    // share of the speed lost a step (besides the air)
constexpr float k_still = 2e-5f;      // metres a step: not moving
constexpr u32 k_still_steps = 60;     // steps that still before it sleeps
constexpr float k_calm = 0.05f;       // m/s: wind this light lets it sleep
constexpr float k_spring_slack = 1.1f;  // and no spring ends a step longer than this, if it can be helped
constexpr float k_tether_slack = 1.05f; // a point strays at most this far over its rest distance from a pin

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float clamp_or(float v, float lo, float hi, float otherwise) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : otherwise;
}

u32 side(float metres) {
    const float n = std::round(metres / k_cloth_spacing) + 1.0f;
    return static_cast<u32>(std::clamp(n, 3.0f, static_cast<float>(k_max_cloth_side)));
}

} // namespace

world::ClothSettings clean(const world::ClothSettings& s) {
    const world::ClothSettings d;
    world::ClothSettings c = s;
    c.width = clamp_or(s.width, 0.05f, 20.0f, d.width);
    c.height = clamp_or(s.height, 0.05f, 20.0f, d.height);
    c.weight = clamp_or(s.weight, 0.02f, 5.0f, d.weight);
    c.stiffness = clamp_or(s.stiffness, 0.0f, 1.0f, d.stiffness);
    if (c.pin != world::ClothPin::top && c.pin != world::ClothPin::corners && c.pin != world::ClothPin::left) {
        c.pin = d.pin;
    }
    return c;
}

Cloths::Sheet::Sheet(const world::ClothSettings& settings) : m_settings(clean(settings)) {
    const world::ClothSettings& s = m_settings;
    m_columns = side(s.width);
    m_rows = side(s.height);
    const u32 n = m_columns * m_rows;
    const float dx = s.width / static_cast<float>(m_columns - 1);
    const float dy = s.height / static_cast<float>(m_rows - 1);
    m_point_area = s.width * s.height / static_cast<float>(n);
    m_rest.resize(n);
    m_inverse_mass.resize(n);
    m_from_pin.resize(n);
    const float inverse_mass = 1.0f / (s.weight * m_point_area);
    for (u32 r = 0; r < m_rows; ++r) {
        for (u32 c = 0; c < m_columns; ++c) {
            const u32 i = r * m_columns + c;
            const float x = -0.5f * s.width + dx * static_cast<float>(c);
            const float y = -dy * static_cast<float>(r);
            m_rest[i] = {x, y, 0.0f};
            bool pin = false;
            switch (s.pin) {
            case world::ClothPin::top: pin = r == 0; break;
            case world::ClothPin::corners: pin = r == 0 && (c == 0 || c == m_columns - 1); break;
            case world::ClothPin::left: pin = c == 0; break;
            }
            m_inverse_mass[i] = pin ? 0.0f : inverse_mass;
            m_from_pin[i] = s.pin == world::ClothPin::left ? x + 0.5f * s.width : -y;
        }
    }
    // Springs: along the grid (holding its size), across each square (its
    // shape) and over two points (its folds), softer the softer the cloth.
    const float shear = 0.25f + 0.5f * s.stiffness;
    const float bend = 0.02f + 0.5f * s.stiffness;
    const auto spring = [this](u32 c0, u32 r0, u32 c1, u32 r1, float k) {
        if (c1 >= m_columns || r1 >= m_rows) return;
        const u32 a = r0 * m_columns + c0, b = r1 * m_columns + c1;
        if (m_inverse_mass[a] == 0.0f && m_inverse_mass[b] == 0.0f) return;
        const float wa = m_inverse_mass[a], wb = m_inverse_mass[b];
        m_springs.push_back({a, b, length(m_rest[b] - m_rest[a]), k * wa / (wa + wb), k * wb / (wa + wb)});
    };
    for (u32 r = 0; r < m_rows; ++r) {
        for (u32 c = 0; c < m_columns; ++c) {
            spring(c, r, c + 1, r, 1.0f);
            spring(c, r, c, r + 1, 1.0f);
        }
    }
    for (u32 r = 0; r < m_rows; ++r) {
        for (u32 c = 0; c < m_columns; ++c) {
            spring(c, r, c + 1, r + 1, shear);
            if (c > 0) spring(c, r, c - 1, r + 1, shear);
            spring(c, r, c + 2, r, bend);
            spring(c, r, c, r + 2, bend);
        }
    }
    // Tethers: each free point stays within its rest distance (a hair
    // over) of the pins holding it (its column's top, its row's pole, or
    // both corners), so however hard it blows the sheet never stretches far
    // from what holds it; the springs alone give way under a storm.
    std::vector<u32> pins;
    for (u32 i = 0; i < n; ++i) {
        if (m_inverse_mass[i] == 0.0f) pins.push_back(i);
    }
    for (u32 i = 0; i < n; ++i) {
        if (m_inverse_mass[i] == 0.0f) continue;
        const u32 r = i / m_columns, c = i % m_columns;
        switch (s.pin) {
        case world::ClothPin::top: m_tethers.push_back({c, i, length(m_rest[i] - m_rest[c])}); break;
        case world::ClothPin::left: {
            const u32 at = r * m_columns;
            m_tethers.push_back({at, i, length(m_rest[i] - m_rest[at])});
            break;
        }
        case world::ClothPin::corners:
            for (const u32 at : pins) m_tethers.push_back({at, i, length(m_rest[i] - m_rest[at])});
            break;
        }
    }
    reset();
}

void Cloths::Sheet::reset() {
    m_points = m_rest;
    m_previous = m_rest;
    m_normals.assign(m_rest.size(), Vec3{0.0f, 0.0f, 1.0f});
    m_asleep = false;
    m_still_steps = 0;
    m_time = 0.0f;
    bound();
    ++m_version;
}

void Cloths::Sheet::normals() {
    const u32 w = m_columns, h = m_rows;
    for (u32 r = 0; r < h; ++r) {
        for (u32 c = 0; c < w; ++c) {
            const Vec3 across = m_points[r * w + std::min(c + 1, w - 1)] - m_points[r * w + (c > 0 ? c - 1 : 0)];
            const Vec3 up = m_points[(r > 0 ? r - 1 : 0) * w + c] - m_points[std::min(r + 1, h - 1) * w + c];
            const Vec3 n = normalize_or_zero(cross(across, up));
            m_normals[r * w + c] = length_squared(n) > 0.0f ? n : Vec3{0.0f, 0.0f, 1.0f};
        }
    }
}

void Cloths::Sheet::bound() {
    Vec3 lo = m_points[0], hi = m_points[0];
    for (const Vec3& p : m_points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    m_center = (lo + hi) * 0.5f;
    float r2 = 0.0f;
    for (const Vec3& p : m_points) r2 = std::max(r2, length_squared(p - m_center));
    // A little over: the drawn sheet's corners must stay inside.
    m_radius = std::sqrt(r2) * 1.001f + 1e-3f;
}

float Cloths::Sheet::advance(float dt, const Vec3& gravity, const Vec3& wind, float ripple_clock, float air_density,
                             u32 iterations) {
    const float spacing = std::min(m_settings.width / static_cast<float>(m_columns - 1),
                                   m_settings.height / static_cast<float>(m_rows - 1));
    // No point moves further in a step than about half the spacing, so
    // even a storm's push never tears the grid through itself.
    const float max_move = 0.5f * spacing;
    const float max_accel = max_move / (dt * dt);
    const float q = 0.5f * air_density * m_point_area; // dynamic pressure per (m/s)², times area
    const float speed = length(wind);
    const float ripple_waves = 1.0f / std::max(0.6f, 8.0f * spacing); // waves per metre
    const usize n = m_points.size();
    for (usize i = 0; i < n; ++i) {
        const float im = m_inverse_mass[i];
        if (im == 0.0f) continue;
        const Vec3 x = m_points[i];
        const Vec3 v = (x - m_previous[i]) * (1.0f / dt);
        const Vec3 rel = wind - v;
        const Vec3 nrm = m_normals[i];
        const float dn = dot(rel, nrm);
        Vec3 force = nrm * (k_drag * q * dn * std::fabs(dn)) + rel * (k_skin_drag * q * length(rel));
        // Ripples run away from what holds it, as the air sheds off it.
        const float wave = std::sin(k_tau * (ripple_clock - m_from_pin[i] * ripple_waves));
        force = force + nrm * (k_ripple * q * speed * speed * wave);
        Vec3 accel = gravity + force * im;
        const float a2 = length_squared(accel);
        if (a2 > max_accel * max_accel) accel = accel * (max_accel / std::sqrt(a2));
        Vec3 next = x + (x - m_previous[i]) * (1.0f - k_damping) + accel * (dt * dt);
        const Vec3 moved = next - x;
        const float m2 = length_squared(moved);
        if (m2 > max_move * max_move) next = x + moved * (max_move / std::sqrt(m2));
        m_previous[i] = x;
        m_points[i] = next;
    }
    Vec3* const p = m_points.data();
    for (u32 k = 0; k < iterations; ++k) {
        for (const Spring& s : m_springs) {
            const Vec3 d = p[s.b] - p[s.a];
            const float len2 = length_squared(d);
            if (len2 <= 1e-18f) continue;
            const Vec3 fix = d * (1.0f - s.length / std::sqrt(len2));
            p[s.a] = p[s.a] + fix * s.share_a;
            p[s.b] = p[s.b] - fix * s.share_b;
        }
        for (const Tether& t : m_tethers) {
            const Vec3 d = p[t.point] - p[t.pin];
            const float len2 = length_squared(d);
            const float most = t.length * k_tether_slack;
            if (len2 > most * most) p[t.point] = p[t.pin] + d * (most / std::sqrt(len2));
        }
    }
    // Last, no spring left much longer than it is (a storm along the face
    // pulls the grid out sideways, past what the tethers hold).
    for (u32 k = 0; k < 2; ++k) {
        for (const Spring& s : m_springs) {
            const Vec3 d = p[s.b] - p[s.a];
            const float len2 = length_squared(d);
            const float most = s.length * k_spring_slack;
            if (len2 <= most * most) continue;
            const float sum = s.share_a + s.share_b;
            const Vec3 fix = d * (1.0f - most / std::sqrt(len2));
            p[s.a] = p[s.a] + fix * (s.share_a / sum);
            p[s.b] = p[s.b] - fix * (s.share_b / sum);
        }
    }
    float most = 0.0f;
    for (usize i = 0; i < n; ++i) {
        if (!finite(m_points[i])) {
            return -1.0f; // gone wrong: the caller puts it back at rest
        }
        most = std::max(most, length_squared(m_points[i] - m_previous[i]));
    }
    normals(); // for the next step's push and the pose
    return std::sqrt(most);
}

void Cloths::Sheet::skin(std::vector<Mat4>& out) const {
    const u32 w = m_columns, h = m_rows;
    out.resize(m_points.size());
    for (u32 r = 0; r < h; ++r) {
        for (u32 c = 0; c < w; ++c) {
            const u32 i = r * w + c;
            // Its patch's frame: across (x at rest), up (y) and out (z).
            const Vec3 across = m_points[r * w + std::min(c + 1, w - 1)] - m_points[r * w + (c > 0 ? c - 1 : 0)];
            Vec3 x = normalize_or_zero(across);
            Vec3 z = m_normals[i];
            if (length_squared(x) == 0.0f || std::fabs(dot(x, z)) > 0.999f) {
                x = Vec3{1.0f, 0.0f, 0.0f};
                z = Vec3{0.0f, 0.0f, 1.0f};
            }
            x = normalize_or_zero(x - z * dot(x, z));
            const Vec3 y = cross(z, x);
            const Vec3 p = m_points[i];
            const Vec3 rest = m_rest[i];
            Mat4 m;
            const Vec3 cols[3] = {x, y, z};
            for (int k = 0; k < 3; ++k) {
                m.at(0, k) = cols[k].x;
                m.at(1, k) = cols[k].y;
                m.at(2, k) = cols[k].z;
            }
            // p = R * rest + t.
            const Vec3 turned = x * rest.x + y * rest.y + z * rest.z;
            m.at(0, 3) = p.x - turned.x;
            m.at(1, 3) = p.y - turned.y;
            m.at(2, 3) = p.z - turned.z;
            out[i] = m;
        }
    }
}

void Cloths::set(world::EntityId id, const world::ClothSettings& settings, const world::Transform& place) {
    const world::ClothSettings s = clean(settings);
    auto it = m_sheets.find(id);
    if (it == m_sheets.end() || !(it->second.settings() == s)) {
        Sheet fresh(s);
        // Its version moves on from the sheet it replaces: never the same twice.
        if (it != m_sheets.end()) fresh.m_version = it->second.m_version + 1;
        m_sheets.insert_or_assign(id, std::move(fresh));
        it = m_sheets.find(id);
    }
    world::Transform t = place;
    if (!finite(Vec3{static_cast<float>(t.position.x), static_cast<float>(t.position.y),
                     static_cast<float>(t.position.z)})) {
        t.position = {};
    }
    t.rotation = normalize(t.rotation);
    if (!std::isfinite(t.rotation.w) || !std::isfinite(t.rotation.x) || !std::isfinite(t.rotation.y) ||
        !std::isfinite(t.rotation.z)) {
        t.rotation = {};
    }
    auto p = m_places.find(id);
    if (p == m_places.end() || !(p->second.position == t.position && p->second.rotation == t.rotation)) {
        m_places[id] = t;
        it->second.wake();
    }
}

void Cloths::remove(world::EntityId id) {
    m_sheets.erase(id);
    m_places.erase(id);
}

void Cloths::clear() {
    m_sheets.clear();
    m_places.clear();
}

void Cloths::step(double seconds, const WindState& wind, const DVec3& camera, JobSystem* jobs) {
    m_stats.simulated = 0;
    m_stats.asleep = 0;
    m_stats.steps = 0;
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    const float dt = std::clamp(m_settings.step_seconds, 1.0f / 480.0f, 1.0f / 15.0f);
    const float most = dt * static_cast<float>(std::max(m_settings.max_steps, 1u));
    const double reach = m_settings.distance;
    m_near.clear();
    for (const auto& [id, place] : m_places) {
        const DVec3 d = place.position - camera;
        const double d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (d2 <= reach * reach) m_near.push_back({d2, id});
    }
    std::sort(m_near.begin(), m_near.end());
    m_moving.clear();
    m_wind.clear();
    m_down.clear();
    u32 points = 0;
    for (const auto& [d2, id] : m_near) {
        Sheet& s = m_sheets.at(id);
        if (s.m_asleep && m_moving.size() >= m_settings.max_simulated) {
            ++m_stats.asleep;
            continue;
        }
        if (m_moving.size() >= m_settings.max_simulated) continue;
        const world::Transform& place = m_places.at(id);
        const Quat back = conjugate(place.rotation);
        // The wind where the sheet hangs, in its space.
        const Vec3 local_wind = rotate(back, wind_at(wind, place.position));
        if (length(local_wind) > k_calm) s.wake();
        if (s.m_asleep) {
            ++m_stats.asleep;
            continue;
        }
        const u32 count = static_cast<u32>(s.m_points.size());
        if (!m_moving.empty() && points + count > m_settings.max_points) continue;
        points += count;
        m_moving.push_back(&s);
        m_wind.push_back(local_wind);
        m_down.push_back(rotate(back, Vec3{0.0f, -m_settings.gravity, 0.0f}));
    }
    // Sheets out of reach drop the time they would have stepped.
    for (auto& [id, s] : m_sheets) s.m_time = std::min(s.m_time, dt);
    const float speed = length(wind.velocity);
    const float ripple_rate = 1.0f + 0.15f * speed; // turns a second, as WindClock's flutter
    const float added = static_cast<float>(std::min(seconds, static_cast<double>(most)));
    const u32 iterations = std::clamp(m_settings.iterations, 1u, 64u);
    const float air = clamp_or(m_settings.air_density, 0.0f, 10.0f, 1.2f);
    std::vector<u32>& steps = m_steps;
    std::vector<u8>& broke = m_broke;
    steps.assign(m_moving.size(), 0u);
    broke.assign(m_moving.size(), 0u);
    const auto run = [&](usize begin, usize end) {
        for (usize k = begin; k < end; ++k) {
            Sheet& s = *m_moving[k];
            s.m_time = std::min(s.m_time + added, most);
            float moved = 0.0f;
            u32 taken = 0;
            // The steps end now (the wind's clock): the last one at the
            // clock, those before it a step's worth earlier each.
            const u32 due = static_cast<u32>(s.m_time / dt);
            while (s.m_time >= dt) {
                s.m_time -= dt;
                const float ahead = static_cast<float>(due > taken + 1 ? due - taken - 1 : 0u);
                const float clock = wind.flutter_clock - ripple_rate * dt * ahead;
                const float m = s.advance(dt, m_down[k], m_wind[k], clock, air, iterations);
                ++taken;
                if (m < 0.0f) {
                    broke[k] = 1;
                    s.reset();
                    break;
                }
                moved = std::max(moved, m);
                if (m < k_still) {
                    if (++s.m_still_steps >= k_still_steps && length(m_wind[k]) <= k_calm) {
                        s.m_asleep = true;
                        break;
                    }
                } else {
                    s.m_still_steps = 0;
                }
            }
            steps[k] = taken;
            if (taken > 0 && broke[k] == 0) {
                s.bound();
                if (moved > 0.0f) ++s.m_version;
            }
        }
    };
    if (jobs != nullptr && m_moving.size() > 1) {
        jobs->parallel_for(m_moving.size(), 1, run);
    } else {
        run(0, m_moving.size());
    }
    m_stats.simulated = static_cast<u32>(m_moving.size());
    for (usize k = 0; k < m_moving.size(); ++k) {
        m_stats.steps += steps[k];
        m_stats.resets += broke[k];
    }
}

void Cloths::push(const DVec3& at, float radius, const Vec3& velocity) {
    if (!std::isfinite(radius) || radius <= 0.0f || !finite(velocity) || !std::isfinite(at.x) ||
        !std::isfinite(at.y) || !std::isfinite(at.z)) {
        return;
    }
    const float dt = std::clamp(m_settings.step_seconds, 1.0f / 480.0f, 1.0f / 15.0f);
    for (auto& [id, s] : m_sheets) {
        const world::Transform& place = m_places.at(id);
        const Quat back = conjugate(place.rotation);
        const DVec3 d = at - place.position;
        const Vec3 local = rotate(back, Vec3{static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)});
        if (length(local - s.m_center) > s.m_radius + radius) continue;
        const Vec3 dv = rotate(back, velocity);
        bool any = false;
        for (usize i = 0; i < s.m_points.size(); ++i) {
            if (s.m_inverse_mass[i] == 0.0f) continue;
            const float dist = length(s.m_points[i] - local);
            if (dist >= radius) continue;
            const float share = 1.0f - dist / radius;
            // A change of speed is a change of where it was a step ago.
            s.m_previous[i] = s.m_previous[i] - dv * (share * dt);
            any = true;
        }
        if (any) s.wake();
    }
}

const Cloths::Sheet* Cloths::find(world::EntityId id) const {
    const auto it = m_sheets.find(id);
    return it == m_sheets.end() ? nullptr : &it->second;
}

ClothStats Cloths::stats() const {
    ClothStats s = m_stats;
    s.sheets = static_cast<u32>(m_sheets.size());
    return s;
}

} // namespace giant::sim
