#include "water_meshes.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace giant::editor {

namespace {

// Twice the signed area of a, b, c (x, z) measured from `o` (kept near the
// points, so far from the origin the products stay exact enough).
double cross(const world::WaterPoint& o, const world::WaterPoint& a, const world::WaterPoint& b,
             const world::WaterPoint& c) {
    const double ax = a.x - o.x, az = a.z - o.z;
    const double bx = b.x - o.x, bz = b.z - o.z;
    const double cx = c.x - o.x, cz = c.z - o.z;
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

// Whether any two edges of the closed shore that are not neighbours meet
// (a sweep along x: edges sorted by their least x, each against those
// starting before it ends). Touching counts.
bool crosses_itself(std::span<const world::WaterPoint> p, const world::WaterPoint& o, double straight) {
    const usize n = p.size();
    std::vector<u32> order(n);
    std::iota(order.begin(), order.end(), 0u);
    const auto lo = [&](u32 e) { return std::min(p[e].x, p[(e + 1) % n].x); };
    const auto hi = [&](u32 e) { return std::max(p[e].x, p[(e + 1) % n].x); };
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return lo(a) < lo(b); });
    const auto side = [&](const world::WaterPoint& a, const world::WaterPoint& b, const world::WaterPoint& c) {
        const double s = cross(o, a, b, c);
        return s > straight ? 1 : (s < -straight ? -1 : 0);
    };
    const auto on = [&](const world::WaterPoint& a, const world::WaterPoint& b, const world::WaterPoint& c) {
        // c on the segment a-b (already known in line with it).
        return std::min(a.x, b.x) <= c.x && c.x <= std::max(a.x, b.x) && std::min(a.z, b.z) <= c.z &&
               c.z <= std::max(a.z, b.z);
    };
    for (usize i = 0; i < n; ++i) {
        const u32 e = order[i];
        const world::WaterPoint& a = p[e];
        const world::WaterPoint& b = p[(e + 1) % n];
        const double end = hi(e);
        for (usize j = i + 1; j < n && lo(order[j]) <= end; ++j) {
            const u32 f = order[j];
            if (f == (e + 1) % n || e == (f + 1) % n) continue; // neighbours share a point
            const world::WaterPoint& c = p[f];
            const world::WaterPoint& d = p[(f + 1) % n];
            if (std::max(c.z, d.z) < std::min(a.z, b.z) || std::max(a.z, b.z) < std::min(c.z, d.z)) continue;
            const int s1 = side(a, b, c), s2 = side(a, b, d), s3 = side(c, d, a), s4 = side(c, d, b);
            if (s1 * s2 < 0 && s3 * s4 < 0) return true;
            if ((s1 == 0 && on(a, b, c)) || (s2 == 0 && on(a, b, d)) || (s3 == 0 && on(c, d, a)) ||
                (s4 == 0 && on(c, d, b))) {
                return true;
            }
        }
    }
    return false;
}

render::WaterLook look_of(const world::Water& w) {
    render::WaterLook l;
    l.color = w.color;
    l.absorption = w.absorption;
    l.turbidity = w.turbidity;
    l.waves = w.waves;
    return l;
}

render::WaterVertex vertex(double x, double y, double z, float flow_x, float flow_z) {
    render::WaterVertex v;
    v.position = DVec3{x, y, z};
    v.flow_x = flow_x;
    v.flow_z = flow_z;
    return v;
}

// The height a body is put in order by (a river: its highest point).
double order_level(const world::Water& w) {
    if (w.kind != world::WaterKind::river || w.path.empty()) return w.level;
    float top = w.path[0].level;
    for (const world::RiverPoint& p : w.path) top = std::max(top, p.level);
    return top;
}

} // namespace

std::vector<u32> clip_ears(std::span<const world::WaterPoint> given, bool& whole) {
    whole = false;
    std::vector<u32> out;
    // Points on top of the one before dropped first (else their edges, of no
    // length, would touch the shore's next ones and read as crossing).
    std::vector<u32> kept;
    std::vector<world::WaterPoint> points;
    for (usize i = 0; i < given.size(); ++i) {
        if (!points.empty() && given[i] == points.back()) continue;
        kept.push_back(static_cast<u32>(i));
        points.push_back(given[i]);
    }
    while (points.size() > 1 && points.back() == points.front()) {
        points.pop_back();
        kept.pop_back();
    }
    const usize n = points.size();
    if (n < 3) return out;
    const world::WaterPoint& o = points[0];
    // Counter-clockwise (x, z as a right-handed plane: positive area).
    double area = 0.0;
    for (usize i = 0; i < n; ++i) {
        const world::WaterPoint& a = points[i];
        const world::WaterPoint& b = points[(i + 1) % n];
        area += (a.x - o.x) * (b.z - o.z) - (b.x - o.x) * (a.z - o.z);
    }
    if (!(std::fabs(area) > 0.0) || !std::isfinite(area)) return out;
    // In a straight line within this much (a fraction of the shore's size squared).
    const double straight = std::fabs(area) * 1e-12;
    if (crosses_itself(points, o, straight)) return out;
    whole = true;
    // The shore as a ring (counter-clockwise), with which corners turn back.
    std::vector<u32> id(n), prev(n), next(n);
    for (usize k = 0; k < n; ++k) {
        id[k] = static_cast<u32>(area > 0.0 ? k : n - 1 - k);
        prev[k] = static_cast<u32>((k + n - 1) % n);
        next[k] = static_cast<u32>((k + 1) % n);
    }
    const auto at = [&](u32 k) -> const world::WaterPoint& { return points[id[k]]; };
    const auto turn = [&](u32 k) { return cross(o, at(prev[k]), at(k), at(next[k])); };
    std::vector<u8> reflex(n, 0);
    usize reflex_count = 0;
    for (u32 k = 0; k < n; ++k) {
        if (turn(k) < 0.0) {
            reflex[k] = 1;
            ++reflex_count;
        }
    }
    // The corners that turn back filed in a grid over the shore (about one a
    // cell), so an ear looks only at those near it.
    double min_x = points[0].x, max_x = min_x, min_z = points[0].z, max_z = min_z;
    for (const world::WaterPoint& p : points) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_z = std::min(min_z, p.z);
        max_z = std::max(max_z, p.z);
    }
    const i32 side = std::clamp(static_cast<i32>(std::sqrt(static_cast<double>(reflex_count))), 1, 64);
    const double cell_x = std::max((max_x - min_x) / side, 1e-300);
    const double cell_z = std::max((max_z - min_z) / side, 1e-300);
    const auto cell_of = [&](double v, double lo, double size) {
        return std::clamp(static_cast<i32>(std::floor((v - lo) / size)), 0, side - 1);
    };
    std::vector<std::vector<u32>> grid(static_cast<usize>(side) * static_cast<usize>(side));
    for (u32 k = 0; k < n; ++k) {
        if (reflex[k] == 0) continue;
        grid[static_cast<usize>(cell_of(at(k).z, min_z, cell_z)) * static_cast<usize>(side) +
             static_cast<usize>(cell_of(at(k).x, min_x, cell_x))]
            .push_back(k);
    }
    const auto remove = [&](u32 k) {
        reflex[k] = 0;
        next[prev[k]] = next[k];
        prev[next[k]] = prev[k];
        // Its neighbours may stop turning back (never start, in a simple shore).
        for (const u32 m : {prev[k], next[k]}) {
            if (reflex[m] != 0 && turn(m) >= 0.0) reflex[m] = 0;
        }
    };
    out.reserve((n - 2) * 3);
    usize left = n;
    u32 k = 0;
    usize since_ear = 0;
    while (left > 3) {
        if (since_ear > left) {
            whole = false; // no ear found round the whole ring (should not happen once checked simple)
            for (u32& i : out) i = kept[i];
            return out;
        }
        const u32 a = prev[k], c = next[k];
        const double t = turn(k);
        if (std::fabs(t) <= straight) {
            // A point in a straight line (or on top of its neighbour): dropped, no triangle.
            remove(k);
            --left;
            k = c;
            since_ear = 0;
            continue;
        }
        bool ear = t > 0.0;
        if (ear) {
            // No corner that turns back (only those can be inside) within it.
            const world::WaterPoint& pa = at(a);
            const world::WaterPoint& pb = at(k);
            const world::WaterPoint& pc = at(c);
            const i32 x0 = cell_of(std::min({pa.x, pb.x, pc.x}), min_x, cell_x);
            const i32 x1 = cell_of(std::max({pa.x, pb.x, pc.x}), min_x, cell_x);
            const i32 z0 = cell_of(std::min({pa.z, pb.z, pc.z}), min_z, cell_z);
            const i32 z1 = cell_of(std::max({pa.z, pb.z, pc.z}), min_z, cell_z);
            for (i32 gz = z0; gz <= z1 && ear; ++gz) {
                for (i32 gx = x0; gx <= x1 && ear; ++gx) {
                    std::vector<u32>& list = grid[static_cast<usize>(gz) * static_cast<usize>(side) + static_cast<usize>(gx)];
                    usize keep = 0;
                    for (usize r = 0; r < list.size(); ++r) {
                        const u32 q = list[r];
                        if (reflex[q] == 0) continue; // no longer turns back (or gone): dropped
                        list[keep++] = q;
                        if (!ear || q == a || q == c) continue;
                        const world::WaterPoint& p = at(q);
                        if (cross(o, pa, pb, p) >= 0.0 && cross(o, pb, pc, p) >= 0.0 && cross(o, pc, pa, p) >= 0.0) {
                            ear = false;
                        }
                    }
                    list.resize(keep);
                }
            }
        }
        if (ear) {
            out.insert(out.end(), {id[a], id[k], id[c]});
            remove(k);
            --left;
            k = c;
            since_ear = 0;
            continue;
        }
        k = c;
        ++since_ear;
    }
    if (std::fabs(turn(k)) > straight) out.insert(out.end(), {id[prev[k]], id[k], id[next[k]]});
    for (u32& i : out) i = kept[i];
    return out;
}

void WaterMeshes::update(std::span<const world::Water> water, const DVec3& camera) {
    if (!m_built || !std::ranges::equal(water, m_seen)) {
        m_seen.assign(water.begin(), water.end());
        m_built = true;
        std::vector<Body> bodies;
        bodies.reserve(water.size());
        std::vector<u8> taken(m_bodies.size(), 0);
        for (usize i = 0; i < water.size(); ++i) {
            const world::Water& w = water[i];
            // Kept if its shape is the same as before (where it was, or by
            // name): its look and a lake's level are put on when put together.
            const auto same_shape = [&](const Body& b) {
                return b.water.kind == w.kind && b.water.visible == w.visible && b.water.outline == w.outline &&
                       b.water.path == w.path && b.water.flow == w.flow;
            };
            usize kept = taken.size();
            if (i < m_bodies.size() && taken[i] == 0 && same_shape(m_bodies[i])) kept = i;
            for (usize j = 0; kept == taken.size() && j < m_bodies.size(); ++j) {
                if (taken[j] == 0 && m_bodies[j].water.name == w.name && same_shape(m_bodies[j])) kept = j;
            }
            if (kept != taken.size()) {
                taken[kept] = 1;
                bodies.push_back(std::move(m_bodies[kept]));
                bodies.back().water = w;
                continue;
            }
            Body b;
            b.water = w;
            if (!w.visible) {
                // Hidden: nothing to cut until shown.
            } else if (w.kind == world::WaterKind::lake) {
                ++m_rebuilds;
                bool whole = true;
                b.indices = clip_ears(w.outline, whole);
                b.broken = !whole; // (crossing, or no area: not drawn)
                if (b.broken) b.indices.clear();
                if (!b.broken) {
                    for (const world::WaterPoint& p : w.outline) b.vertices.push_back(vertex(p.x, w.level, p.z, 0.0f, 0.0f));
                }
            } else if (w.kind == world::WaterKind::river && w.path.size() >= 2) {
                ++m_rebuilds;
                const usize n = w.path.size();
                const auto dir = [&](usize from, usize to, double& dx, double& dz) {
                    dx = w.path[to].x - w.path[from].x;
                    dz = w.path[to].z - w.path[from].z;
                    const double len = std::sqrt(dx * dx + dz * dz);
                    if (len > 0.0) {
                        dx /= len;
                        dz /= len;
                    } else {
                        dx = 0.0;
                        dz = 0.0;
                    }
                };
                for (usize p = 0; p < n; ++p) {
                    // Along: the two stretches' directions halved; across, square to
                    // that, widened at a bend so each stretch keeps its width (at
                    // most doubled at the sharpest).
                    double ix = 0.0, iz = 0.0, ox = 0.0, oz = 0.0;
                    if (p > 0) dir(p - 1, p, ix, iz);
                    if (p + 1 < n) dir(p, p + 1, ox, oz);
                    double tx = ix + ox, tz = iz + oz;
                    double len = std::sqrt(tx * tx + tz * tz);
                    if (!(len > 1e-9)) {
                        tx = ox != 0.0 || oz != 0.0 ? ox : (ix != 0.0 || iz != 0.0 ? ix : 1.0);
                        tz = ox != 0.0 || oz != 0.0 ? oz : (ix != 0.0 || iz != 0.0 ? iz : 0.0);
                        len = std::sqrt(tx * tx + tz * tz);
                    }
                    tx /= len;
                    tz /= len;
                    const bool out_known = ox != 0.0 || oz != 0.0;
                    const bool in_known = ix != 0.0 || iz != 0.0;
                    const double along = out_known ? tx * ox + tz * oz : (in_known ? tx * ix + tz * iz : 1.0);
                    const double widen = 1.0 / std::max(along, 0.5);
                    const world::RiverPoint& rp = w.path[p];
                    const double half = 0.5 * std::max(rp.width, 0.0f) * widen;
                    const float fx = static_cast<float>(tx) * w.flow;
                    const float fz = static_cast<float>(tz) * w.flow;
                    b.vertices.push_back(vertex(rp.x - tz * half, rp.level, rp.z + tx * half, fx, fz));
                    b.vertices.push_back(vertex(rp.x + tz * half, rp.level, rp.z - tx * half, fx, fz));
                }
                for (usize p = 0; p + 1 < n; ++p) {
                    const u32 a = static_cast<u32>(p * 2);
                    b.indices.insert(b.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
                }
            }
            bodies.push_back(std::move(b));
        }
        m_bodies = std::move(bodies);
        assemble();
    }
    place_seas(camera);
}

void WaterMeshes::assemble() {
    m_vertices.clear();
    m_indices.clear();
    m_looks.clear();
    m_seas.clear();
    m_broken = 0;
    if (m_radii.empty()) {
        // Rings growing by half again each, from 4 m out to k_sea_radius, then the edge.
        m_radii.push_back(0.0);
        for (double r = 4.0; m_radii.back() < k_sea_radius; r *= 1.5) m_radii.push_back(std::min(r, k_sea_radius));
        m_radii.push_back(k_sea_edge);
    }
    // Looks in the scene's order; bodies lowest first.
    std::vector<u32> look(m_bodies.size(), 0);
    std::vector<usize> order;
    for (usize i = 0; i < m_bodies.size(); ++i) {
        const world::Water& w = m_bodies[i].water;
        if (!w.visible || m_looks.size() >= render::k_max_water_looks) continue;
        if (m_bodies[i].broken) ++m_broken;
        look[i] = static_cast<u32>(m_looks.size());
        m_looks.push_back(look_of(w));
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](usize a, usize b) { return order_level(m_bodies[a].water) < order_level(m_bodies[b].water); });
    for (const usize i : order) {
        const Body& b = m_bodies[i];
        const u32 base = static_cast<u32>(m_vertices.size());
        if (b.water.kind == world::WaterKind::sea) {
            m_seas.push_back({b.water.level, look[i], m_vertices.size()});
            const usize rings = m_radii.size() - 1;
            m_vertices.resize(m_vertices.size() + 1 + rings * k_sea_segments); // placed by place_seas
            const auto at = [&](usize ring, u32 s) {
                return ring == 0 ? base : base + 1 + static_cast<u32>(ring - 1) * k_sea_segments + (s % k_sea_segments);
            };
            for (u32 s = 0; s < k_sea_segments; ++s) m_indices.insert(m_indices.end(), {at(0, 0), at(1, s), at(1, s + 1)});
            for (usize ring = 1; ring < rings; ++ring) {
                for (u32 s = 0; s < k_sea_segments; ++s) {
                    m_indices.insert(m_indices.end(), {at(ring, s), at(ring + 1, s), at(ring + 1, s + 1)});
                    m_indices.insert(m_indices.end(), {at(ring, s), at(ring + 1, s + 1), at(ring, s + 1)});
                }
            }
            continue;
        }
        const bool lake = b.water.kind == world::WaterKind::lake;
        for (render::WaterVertex v : b.vertices) {
            v.look = look[i];
            if (lake) v.position.y = b.water.level;
            m_vertices.push_back(v);
        }
        for (const u32 t : b.indices) m_indices.push_back(base + t);
    }
}

void WaterMeshes::place_seas(const DVec3& camera) {
    // (A camera that is not a number: the rings where they were.)
    if (!std::isfinite(camera.x) || !std::isfinite(camera.z)) return;
    for (const Sea& sea : m_seas) {
        render::WaterVertex* v = m_vertices.data() + sea.first_vertex;
        *v++ = [&] {
            render::WaterVertex c = vertex(camera.x, sea.level, camera.z, 0.0f, 0.0f);
            c.look = sea.look;
            return c;
        }();
        for (usize ring = 1; ring < m_radii.size(); ++ring) {
            for (u32 s = 0; s < k_sea_segments; ++s) {
                const double angle = 6.283185307179586 * s / k_sea_segments;
                *v = vertex(camera.x + m_radii[ring] * std::cos(angle), sea.level,
                            camera.z + m_radii[ring] * std::sin(angle), 0.0f, 0.0f);
                v->look = sea.look;
                ++v;
            }
        }
    }
}

} // namespace giant::editor
