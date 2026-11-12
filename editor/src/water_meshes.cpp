#include "water_meshes.h"

#include "giant/world/water.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

render::WaterLook water_look(const world::Water& w) {
    render::WaterLook l;
    l.color = w.color;
    l.absorption = w.absorption;
    l.turbidity = w.turbidity;
    l.waves = world::swell_scale(w); // (0 to 4; a bad number: still, as physics has it)
    return l;
}

render::Underwater camera_water(std::span<const world::Water> water, const DVec3& camera, float swell_most) {
    render::Underwater u;
    // Over every surface (with room for the near plane): none, and no search.
    const double crest = std::isfinite(swell_most) ? std::max(static_cast<double>(swell_most), 0.0) : 0.0;
    double highest = -std::numeric_limits<double>::infinity();
    for (const world::Water& w : water) {
        if (!w.visible) continue;
        if (w.kind == world::WaterKind::river) {
            for (const world::RiverPoint& p : w.path) highest = std::max(highest, static_cast<double>(p.level));
        } else {
            const double lift = w.kind == world::WaterKind::sea ? crest * world::swell_scale(w) : 0.0;
            highest = std::max(highest, static_cast<double>(w.level) + lift);
        }
    }
    if (!(camera.y < highest + k_camera_water_margin)) return u;
    const std::optional<world::WaterHere> here = world::water_at(water, camera.x, camera.z);
    if (!here || here->water == nullptr) return u;
    u.on = true;
    u.level = here->level;
    u.look = water_look(*here->water);
    u.swell = here->water->kind == world::WaterKind::sea;
    return u;
}

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

void WaterMeshes::update(std::span<const world::Water> water) {
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
                // Each point's two banks (across, square to its two stretches'
                // directions halved, widened at a bend so each stretch keeps its
                // width, at most doubled at the sharpest), its flow, and how
                // its surface slopes (downstream, over both its stretches).
                struct Bank {
                    double lx, lz, rx, rz;
                    double tx, tz; // downstream
                    float fx, fz, slope_x, slope_z;
                };
                // A stretch's own slope (rise a metre downstream; a waterfall's the steepest).
                const auto stretch_slope = [&](usize a, double& slope) {
                    const double run = std::hypot(w.path[a + 1].x - w.path[a].x, w.path[a + 1].z - w.path[a].z);
                    const double rise = static_cast<double>(w.path[a + 1].level) - w.path[a].level;
                    if (run > 1e-6) {
                        slope = std::clamp(rise / run, -k_slope_most, k_slope_most);
                        return true;
                    }
                    slope = rise > 0.0 ? k_slope_most : (rise < 0.0 ? -k_slope_most : 0.0);
                    return false; // (no run: no way along it)
                };
                std::vector<Bank> banks(n);
                for (usize p = 0; p < n; ++p) {
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
                    // The point's slope: its stretches' own, averaged (a waterfall
                    // beside it, of no run, set on that stretch's corners below).
                    double sum = 0.0;
                    int stretches = 0;
                    for (const usize a : {p > 0 ? p - 1 : n, p + 1 < n ? p : n}) {
                        double s = 0.0;
                        if (a < n && stretch_slope(a, s)) {
                            sum += s;
                            ++stretches;
                        }
                    }
                    const double slope = stretches > 0 ? sum / stretches : 0.0;
                    banks[p] = {rp.x - tz * half, rp.z + tx * half, rp.x + tz * half, rp.z - tx * half, tx, tz,
                                static_cast<float>(tx) * w.flow, static_cast<float>(tz) * w.flow,
                                static_cast<float>(tx * slope), static_cast<float>(tz * slope)};
                }
                // Each stretch its own four corners, so its rapids stay on it
                // (where its surface falls steeply; a waterfall all white).
                for (usize p = 0; p + 1 < n; ++p) {
                    const world::RiverPoint& a = w.path[p];
                    const world::RiverPoint& c = w.path[p + 1];
                    const double run = std::hypot(c.x - a.x, c.z - a.z);
                    const double drop = std::fabs(static_cast<double>(c.level) - a.level);
                    const double fall = drop > 0.0 ? (run > 1e-6 ? drop / run : k_slope_most) : 0.0;
                    const float rapids =
                        static_cast<float>(std::clamp((fall - k_rapids_from) / (k_rapids_full - k_rapids_from), 0.0, 1.0));
                    double own = 0.0;
                    const bool sheer = !stretch_slope(p, own) && own != 0.0; // a waterfall: its own, the steepest
                    const u32 first = static_cast<u32>(b.vertices.size());
                    for (const usize q : {p, p + 1}) {
                        const Bank& k = banks[q];
                        const float level = w.path[q].level;
                        for (const bool left : {true, false}) {
                            render::WaterVertex v = vertex(left ? k.lx : k.rx, level, left ? k.lz : k.rz, k.fx, k.fz);
                            v.rapids = rapids;
                            v.slope_x = sheer ? static_cast<float>(k.tx * own) : k.slope_x;
                            v.slope_z = sheer ? static_cast<float>(k.tz * own) : k.slope_z;
                            b.vertices.push_back(v);
                        }
                    }
                    b.indices.insert(b.indices.end(), {first, first + 2, first + 1, first + 1, first + 2, first + 3});
                }
            }
            bodies.push_back(std::move(b));
        }
        m_bodies = std::move(bodies);
        assemble();
    }
}

void WaterMeshes::assemble() {
    m_vertices.clear();
    m_indices.clear();
    m_looks.clear();
    m_seas.clear();
    m_under_seas = 0;
    m_broken = 0;
    // Looks in the scene's order; bodies lowest first.
    std::vector<u32> look(m_bodies.size(), 0);
    std::vector<usize> order;
    for (usize i = 0; i < m_bodies.size(); ++i) {
        const world::Water& w = m_bodies[i].water;
        if (!w.visible || m_looks.size() >= render::k_max_water_looks) continue;
        if (m_bodies[i].broken) ++m_broken;
        look[i] = static_cast<u32>(m_looks.size());
        m_looks.push_back(water_look(w));
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](usize a, usize b) { return order_level(m_bodies[a].water) < order_level(m_bodies[b].water); });
    for (const usize i : order) {
        const Body& b = m_bodies[i];
        const u32 base = static_cast<u32>(m_vertices.size());
        if (b.water.kind == world::WaterKind::sea) {
            if (m_seas.empty()) m_under_seas = static_cast<u32>(m_indices.size());
            m_seas.push_back({static_cast<double>(b.water.level), look[i]}); // (the renderer draws its rings)
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

} // namespace giant::editor
