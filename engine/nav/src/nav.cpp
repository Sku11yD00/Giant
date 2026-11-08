#include "giant/nav/nav.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace giant::nav {

namespace {

constexpr float k_nan = std::numeric_limits<float>::quiet_NaN();
constexpr u32 k_lookahead = 64; // samples a straight run is looked along when pulling the path straight
// The estimate to the end a hair over: among equal paths the one nearer the
// end is looked at first (far fewer samples on open ground), at most 0.1%
// longer than the shortest.
constexpr float k_tie_break = 1.001f;

u64 new_version() {
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

float clean(float v, float lo, float hi, float otherwise) { return std::isfinite(v) ? std::clamp(v, lo, hi) : otherwise; }

NavSettings cleaned(const NavSettings& s) {
    const NavSettings d;
    NavSettings c;
    c.max_slope_degrees = clean(s.max_slope_degrees, 0.0f, 89.0f, d.max_slope_degrees);
    c.step_height = clean(s.step_height, 0.0f, 10.0f, d.step_height);
    c.clearance = clean(s.clearance, 0.1f, 100.0f, d.clearance);
    c.agent_radius = clean(s.agent_radius, 0.0f, 10.0f, d.agent_radius);
    return c;
}

// How far the box reaches along world y from its centre.
float vertical_reach(const Obstacle& o) {
    const Vec3 x = rotate(o.rotation, Vec3{o.half.x, 0.0f, 0.0f});
    const Vec3 y = rotate(o.rotation, Vec3{0.0f, o.half.y, 0.0f});
    const Vec3 z = rotate(o.rotation, Vec3{0.0f, 0.0f, o.half.z});
    return std::fabs(x.y) + std::fabs(y.y) + std::fabs(z.y);
}

// Whether ground point (x, z) is within the box's footprint grown by `grow`
// (seen in its own frame, level through its centre).
bool over(const Obstacle& o, const Quat& back, double x, double z, float grow) {
    const Vec3 d{static_cast<float>(x - o.center.x), 0.0f, static_cast<float>(z - o.center.z)};
    const Vec3 l = rotate(back, d);
    // The box's own y may lie along the ground (a toppled box): every axis counts.
    return std::fabs(l.x) <= std::fabs(o.half.x) + grow && std::fabs(l.z) <= std::fabs(o.half.z) + grow &&
           std::fabs(l.y) <= std::fabs(o.half.y) + grow;
}

} // namespace

DVec3 NavTile::sample_point(world::CellCoord cell, u32 i, u32 j) {
    return {cell.x * world::k_cell_size + (i + 0.5) * k_nav_spacing, 0.0,
            cell.z * world::k_cell_size + (j + 0.5) * k_nav_spacing};
}

NavTile build_tile(world::CellCoord cell, const world::Terrain& terrain, std::span<const Obstacle> obstacles,
                   const NavSettings& settings) {
    const NavSettings s = cleaned(settings);
    constexpr u32 n = k_nav_side;
    NavTile t;
    t.cell = cell;
    t.version = new_version();
    t.height.assign(usize{n} * n, k_nan);
    t.walkable.assign(usize{n} * n, 0);
    std::vector<u8> raised(usize{n} * n, 0);
    // The ground (one sample past each side, for the slopes at the edges),
    // and how steep it is.
    const double h = k_nav_spacing;
    const float max_gradient = std::tan(degrees_to_radians(s.max_slope_degrees));
    constexpr u32 m = n + 2;
    std::vector<double> ground(usize{m} * m, std::numeric_limits<double>::quiet_NaN());
    for (u32 j = 0; j < m; ++j) {
        for (u32 i = 0; i < m; ++i) {
            const DVec3 p = NavTile::sample_point(cell, i, j) - DVec3{h, 0.0, h};
            const std::optional<double> at = terrain.height_at(p.x, p.z);
            if (at && std::isfinite(*at)) ground[usize{j} * m + i] = *at;
        }
    }
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const usize c = usize{j + 1} * m + (i + 1);
            const double here = ground[c];
            if (!std::isfinite(here)) continue;
            const usize k = usize{j} * n + i;
            t.height[k] = static_cast<float>(here);
            const auto or_here = [&](usize at) { return std::isfinite(ground[at]) ? ground[at] : here; };
            const double dx = (or_here(c + 1) - or_here(c - 1)) / (2.0 * h);
            const double dz = (or_here(c + m) - or_here(c - m)) / (2.0 * h);
            t.walkable[k] = std::sqrt(dx * dx + dz * dz) <= max_gradient ? 1 : 0;
        }
    }
    // The obstacles reaching this tile, with the samples their grown footprints cover.
    const double x0 = cell.x * world::k_cell_size, z0 = cell.z * world::k_cell_size;
    struct Near {
        const Obstacle* o;
        Quat back;
        float top, bottom;
        u32 i0, i1, j0, j1;
    };
    std::vector<Near> near;
    for (const Obstacle& o : obstacles) {
        if (!std::isfinite(o.center.x) || !std::isfinite(o.center.y) || !std::isfinite(o.center.z)) continue;
        const Vec3 a = rotate(o.rotation, Vec3{std::fabs(o.half.x), 0.0f, 0.0f});
        const Vec3 b = rotate(o.rotation, Vec3{0.0f, std::fabs(o.half.y), 0.0f});
        const Vec3 c = rotate(o.rotation, Vec3{0.0f, 0.0f, std::fabs(o.half.z)});
        const double rx = std::fabs(a.x) + std::fabs(b.x) + std::fabs(c.x) + s.agent_radius;
        const double rz = std::fabs(a.z) + std::fabs(b.z) + std::fabs(c.z) + s.agent_radius;
        if (!std::isfinite(rx) || !std::isfinite(rz)) continue;
        const double lo_i = std::floor((o.center.x - rx - x0) / h), hi_i = std::floor((o.center.x + rx - x0) / h);
        const double lo_j = std::floor((o.center.z - rz - z0) / h), hi_j = std::floor((o.center.z + rz - z0) / h);
        if (hi_i < 0 || hi_j < 0 || lo_i >= n || lo_j >= n) continue;
        const float reach = vertical_reach(o);
        near.push_back({&o, conjugate(normalize(o.rotation)), static_cast<float>(o.center.y) + reach,
                        static_cast<float>(o.center.y) - reach, static_cast<u32>(std::max(lo_i, 0.0)),
                        static_cast<u32>(std::min(hi_i, n - 1.0)), static_cast<u32>(std::max(lo_j, 0.0)),
                        static_cast<u32>(std::min(hi_j, n - 1.0))});
    }
    // Lowest top first: a stack of low things is stepped up one by one,
    // whatever order they come in.
    std::stable_sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.top < b.top; });
    // Where there is no ground, the ground is the lowest top over a sample
    // with no lid too low over it (a floor, a deck with no terrain under it;
    // not a footing under the floor). What stands on it (its bottom within a
    // step of the top: a wall) is in the way there, below.
    struct Top {
        usize k;
        float top, bottom;
    };
    std::vector<Top> tops;
    for (const Near& o : near) {
        for (u32 j = o.j0; j <= o.j1; ++j) {
            for (u32 i = o.i0; i <= o.i1; ++i) {
                const usize k = usize{j} * n + i;
                if (std::isfinite(t.height[k])) continue; // ground (perhaps too steep: not a floor)
                const DVec3 p = NavTile::sample_point(cell, i, j);
                if (over(*o.o, o.back, p.x, p.z, 0.0f)) tops.push_back({k, o.top, o.bottom});
            }
        }
    }
    std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b) { return a.k != b.k ? a.k < b.k : a.top < b.top; });
    for (usize a = 0; a < tops.size();) {
        usize b = a;
        while (b < tops.size() && tops[b].k == tops[a].k) ++b;
        for (usize c = a; c < b; ++c) {
            const float g = tops[c].top;
            bool room = true;
            for (usize d = a; d < b && room; ++d) {
                room = d == c || !(tops[d].bottom > g + s.step_height && tops[d].bottom < g + s.clearance);
            }
            if (room) {
                t.height[tops[c].k] = g;
                t.walkable[tops[c].k] = 1;
                raised[tops[c].k] = 1;
                break;
            }
        }
        a = b;
    }
    // ...then low things are stood on (their top within a step of where one stands)...
    for (const Near& o : near) {
        for (u32 j = o.j0; j <= o.j1; ++j) {
            for (u32 i = o.i0; i <= o.i1; ++i) {
                const usize k = usize{j} * n + i;
                const float g = t.height[k];
                if (!(o.top > g - s.clearance) || o.top > g + s.step_height) continue; // NaN ground: never stood on
                const DVec3 p = NavTile::sample_point(cell, i, j);
                if (!over(*o.o, o.back, p.x, p.z, 0.0f)) continue;
                if (o.top > g) {
                    t.height[k] = o.top;
                    raised[k] = 1;
                }
                if (raised[k] != 0) t.walkable[k] = 1; // a floor over steep ground is flat
            }
        }
    }
    // ...then what stands in the way, from a step above where one stands up to head room.
    for (const Near& o : near) {
        for (u32 j = o.j0; j <= o.j1; ++j) {
            for (u32 i = o.i0; i <= o.i1; ++i) {
                const usize k = usize{j} * n + i;
                if (t.walkable[k] == 0) continue;
                const float g = t.height[k];
                if (!(o.top > g + s.step_height) || !(o.bottom < g + s.clearance)) continue;
                const DVec3 p = NavTile::sample_point(cell, i, j);
                if (over(*o.o, o.back, p.x, p.z, s.agent_radius)) t.walkable[k] = 0;
            }
        }
    }
    for (const u8 w : t.walkable) t.walkable_count += w;
    return t;
}

void NavMap::set_tile(NavTile tile) {
    const world::CellCoord c = tile.cell;
    m_tiles[c] = std::move(tile);
    ++m_version;
}

void NavMap::remove_tile(world::CellCoord cell) {
    if (m_tiles.erase(cell) != 0) ++m_version;
}

void NavMap::clear() {
    if (!m_tiles.empty()) ++m_version;
    m_tiles.clear();
}

const NavTile* NavMap::find(world::CellCoord cell) const {
    const auto it = m_tiles.find(cell);
    return it == m_tiles.end() ? nullptr : &it->second;
}

world::CellCoord NavMap::cell_of_sample(i64 gi, i64 gj) {
    const auto div = [](i64 a) {
        const i64 n = k_nav_side;
        const i64 q = a >= 0 ? a / n : -((-a + n - 1) / n);
        return static_cast<i32>(std::clamp<i64>(q, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()));
    };
    return {div(gi), div(gj)};
}

NavMap::Sample NavMap::sample(i64 gi, i64 gj) const {
    const world::CellCoord c = cell_of_sample(gi, gj);
    const NavTile* t = find(c);
    if (t == nullptr) return {};
    const i64 i = gi - i64{c.x} * k_nav_side, j = gj - i64{c.z} * k_nav_side;
    if (i < 0 || j < 0 || i >= k_nav_side || j >= k_nav_side) return {}; // past i32's cells
    const usize k = static_cast<usize>(j) * k_nav_side + static_cast<usize>(i);
    return {true, t->walkable[k] != 0, t->height[k]};
}

const char* to_string(SearchState state) {
    switch (state) {
    case SearchState::searching: return "searching";
    case SearchState::missing: return "missing";
    case SearchState::found: return "found";
    case SearchState::failed: return "failed";
    }
    return "failed";
}

PathSearch::PathSearch(const NavMap& map, const DVec3& from, const DVec3& to, const SearchLimits& limits)
    : m_map(map), m_from(from), m_to(to), m_limits(limits) {
    const auto place = [](const DVec3& p) {
        return std::fabs(p.x) <= k_max_nav_coordinate && std::fabs(p.z) <= k_max_nav_coordinate; // (false for NaN)
    };
    if (!place(from) || !place(to)) {
        m_state = SearchState::failed;
        m_reason = "not a place";
    }
}

bool PathSearch::snap(const DVec3& p, i64& oi, i64& oj, bool& missing, u64& work) {
    missing = false;
    const i64 ci = NavMap::sample_of(p.x), cj = NavMap::sample_of(p.z);
    const float reach = std::isfinite(m_limits.snap_distance) ? std::clamp(m_limits.snap_distance, 0.0f, 20.0f) : 2.0f;
    const i64 r = static_cast<i64>(std::ceil(reach / k_nav_spacing));
    work += static_cast<u64>((2 * r + 1) * (2 * r + 1));
    double best = std::numeric_limits<double>::infinity(), best_dy = 0.0;
    for (i64 dj = -r; dj <= r; ++dj) {
        for (i64 di = -r; di <= r; ++di) {
            const double x = (static_cast<double>(ci + di) + 0.5) * k_nav_spacing - p.x;
            const double z = (static_cast<double>(cj + dj) + 0.5) * k_nav_spacing - p.z;
            const double d = x * x + z * z;
            if (d > (reach + k_nav_spacing) * (reach + k_nav_spacing)) continue;
            const NavMap::Sample s = at(ci + di, cj + dj);
            if (!s.built) {
                // A tile within reach not built yet: waited for (the nearest
                // ground may be there).
                missing = true;
                m_missing = NavMap::cell_of_sample(ci + di, cj + dj);
                return false;
            }
            if (!s.walkable) continue;
            // Nearest; among equals, the one nearest in height.
            const double dy = std::isfinite(p.y) ? std::fabs(s.height - p.y) : 0.0;
            if (d < best || (d == best && dy < best_dy)) {
                best = d;
                best_dy = dy;
                oi = ci + di;
                oj = cj + dj;
            }
        }
    }
    return std::isfinite(best);
}

bool PathSearch::unchanged() {
    if (m_map.version() == m_checked_version) return true;
    for (const auto& [cell, block] : m_block_of) {
        const NavTile* t = m_map.find(cell);
        if ((t != nullptr ? t->version : 0) != m_block_tile[block]) return false;
    }
    m_checked_version = m_map.version();
    return true;
}

void PathSearch::restart() {
    ++m_restarts;
    m_started = false;
    m_phase = Phase::search;
    m_open = {};
    m_blocks.clear();
    m_block_tile.clear();
    m_block_of.clear();
    m_last_block = ~0u;
    m_cells.clear();
    m_path.clear();
}

DVec3 PathSearch::point(i64 i, i64 j) const {
    return {(static_cast<double>(i) + 0.5) * k_nav_spacing, m_map.sample(i, j).height,
            (static_cast<double>(j) + 0.5) * k_nav_spacing};
}

SearchState PathSearch::advance(u32 nodes) {
    if (m_state == SearchState::found || m_state == SearchState::failed) return m_state;
    m_state = SearchState::searching;
    const float step = m_map.settings().step_height;
    const u64 budget = std::max<u32>(nodes, 1);
    u64 looked = 0;
    // A tile it read rebuilt (a wall put down, the ground dug) or dropped:
    // what it knows is stale, so it starts again.
    if (m_started && !unchanged()) restart();
    const auto spent = [&] {
        m_work += looked;
        return m_state;
    };
    if (!m_started) {
        bool missing = false;
        if (!snap(m_from, m_si, m_sj, missing, looked)) {
            m_state = missing ? SearchState::missing : SearchState::failed;
            m_reason = missing ? "" : "no walkable ground near the start";
            return spent();
        }
        if (!snap(m_to, m_gi, m_gj, missing, looked)) {
            m_state = missing ? SearchState::missing : SearchState::failed;
            m_reason = missing ? "" : "no walkable ground near the end";
            return spent();
        }
        m_started = true;
        m_checked_version = m_map.version();
        seen(m_gi, m_gj); // the end's tile watched too
        seen(m_si, m_sj).g = 0.0f;
        m_open.push({0.0f, m_si, m_sj});
    }
    if (m_phase != Phase::search) {
        if (finish(looked, budget)) m_state = SearchState::found;
        return spent();
    }
    const auto heuristic = [&](i64 i, i64 j) {
        const double dx = static_cast<double>(std::llabs(i - m_gi)), dz = static_cast<double>(std::llabs(j - m_gj));
        return static_cast<float>((std::max(dx, dz) + (std::sqrt(2.0) - 1.0) * std::min(dx, dz)) * k_nav_spacing);
    };
    static constexpr int k_di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int k_dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!m_open.empty()) {
        if (looked >= budget) return spent();
        const Node n = m_open.top();
        Seen& here = seen(n.i, n.j);
        if (here.closed != 0) {
            m_open.pop();
            continue;
        }
        if (n.i == m_gi && n.j == m_gj) {
            m_phase = Phase::walk;
            m_wi = m_gi;
            m_wj = m_gj;
            m_cells.clear();
            if (finish(looked, budget)) m_state = SearchState::found;
            return spent();
        }
        const NavMap::Sample hs = at(n.i, n.j);
        // Every neighbour's tile there first (else wait for it, this node
        // kept open); inside a tile, they are.
        const i64 li = n.i - i64{m_tile_cell.x} * k_nav_side, lj = n.j - i64{m_tile_cell.z} * k_nav_side;
        const bool inside = li > 0 && lj > 0 && li < k_nav_side - 1 && lj < k_nav_side - 1;
        for (int d = 0; d < 8 && !inside; ++d) {
            const NavMap::Sample s = at(n.i + k_di[d], n.j + k_dj[d]);
            if (!s.built) {
                m_missing = NavMap::cell_of_sample(n.i + k_di[d], n.j + k_dj[d]);
                m_state = SearchState::missing;
                return spent();
            }
        }
        m_open.pop();
        here.closed = 1;
        const float g = here.g;
        ++looked;
        if (++m_nodes > m_limits.max_nodes) {
            m_state = SearchState::failed;
            m_reason = "too far (searched max_nodes samples)";
            return spent();
        }
        for (int d = 0; d < 8; ++d) {
            const i64 i = n.i + k_di[d], j = n.j + k_dj[d];
            const NavMap::Sample s = at(i, j);
            if (!s.walkable || !(std::fabs(s.height - hs.height) <= step)) continue;
            if (d >= 4) {
                // No corner cut past a blocked sample (or a step too high).
                const NavMap::Sample a = at(n.i + k_di[d], n.j), b = at(n.i, n.j + k_dj[d]);
                if (!a.walkable || !b.walkable || !(std::fabs(a.height - hs.height) <= step) ||
                    !(std::fabs(b.height - hs.height) <= step)) {
                    continue;
                }
            }
            const float run = static_cast<float>((d >= 4 ? std::sqrt(2.0) : 1.0) * k_nav_spacing);
            const float cost = g + run + std::fabs(s.height - hs.height);
            Seen& there = seen(i, j); // (may make a block: `here` is not used after)
            if (there.closed != 0 || there.g <= cost) continue;
            there.g = cost;
            there.from = static_cast<u8>(d);
            m_open.push({cost + heuristic(i, j) * k_tie_break, i, j});
        }
    }
    m_state = SearchState::failed;
    m_reason = "no way there";
    return spent();
}

PathSearch::Seen& PathSearch::seen(i64 i, i64 j) {
    const world::CellCoord c = NavMap::cell_of_sample(i, j);
    if (m_last_block == ~0u || !(c == m_last_cell)) {
        const auto [it, added] = m_block_of.try_emplace(c, static_cast<u32>(m_blocks.size()));
        if (added) {
            m_blocks.emplace_back(usize{k_nav_side} * k_nav_side);
            const NavTile* t = m_map.find(c);
            m_block_tile.push_back(t != nullptr ? t->version : 0);
        }
        m_last_cell = c;
        m_last_block = it->second;
    }
    const i64 li = std::clamp<i64>(i - i64{c.x} * k_nav_side, 0, k_nav_side - 1);
    const i64 lj = std::clamp<i64>(j - i64{c.z} * k_nav_side, 0, k_nav_side - 1);
    return m_blocks[m_last_block][static_cast<usize>(lj) * k_nav_side + static_cast<usize>(li)];
}

NavMap::Sample PathSearch::at(i64 i, i64 j) {
    const world::CellCoord c = NavMap::cell_of_sample(i, j);
    if (m_tile_version != m_map.version() || !(c == m_tile_cell)) {
        m_tile = m_map.find(c);
        m_tile_cell = c;
        m_tile_version = m_map.version();
    }
    if (m_tile == nullptr) return {};
    const i64 li = i - i64{c.x} * k_nav_side, lj = j - i64{c.z} * k_nav_side;
    if (li < 0 || lj < 0 || li >= k_nav_side || lj >= k_nav_side) return {};
    const usize k = static_cast<usize>(lj) * k_nav_side + static_cast<usize>(li);
    return {true, m_tile->walkable[k] != 0, m_tile->height[k]};
}

bool PathSearch::clear_line(i64 ai, i64 aj, i64 bi, i64 bj) const {
    // Every sample the segment between the centres passes through (both at
    // a corner) walkable, no step between them too high.
    const float step = m_map.settings().step_height;
    const double dx = static_cast<double>(bi - ai), dz = static_cast<double>(bj - aj);
    const i64 si = dx > 0 ? 1 : -1, sj = dz > 0 ? 1 : -1;
    const double tx = dx != 0 ? 1.0 / std::fabs(dx) : std::numeric_limits<double>::infinity();
    const double tz = dz != 0 ? 1.0 / std::fabs(dz) : std::numeric_limits<double>::infinity();
    double nx = tx * 0.5, nz = tz * 0.5; // the next boundaries along the segment (0 to 1)
    i64 i = ai, j = aj;
    NavMap::Sample last = m_map.sample(i, j);
    const auto ok = [&](i64 ci, i64 cj, const NavMap::Sample& from) {
        const NavMap::Sample s = m_map.sample(ci, cj);
        return s.walkable && std::fabs(s.height - from.height) <= step;
    };
    while (i != bi || j != bj) {
        if (std::fabs(nx - nz) < 1e-12) {
            // Through a corner: both sides must be clear.
            if (!ok(i + si, j, last) || !ok(i, j + sj, last)) return false;
            i += si;
            j += sj;
            nx += tx;
            nz += tz;
        } else if (nx < nz) {
            i += si;
            nx += tx;
        } else {
            j += sj;
            nz += tz;
        }
        if (!ok(i, j, last)) return false;
        last = m_map.sample(i, j);
    }
    return true;
}

bool PathSearch::finish(u64& looked, u64 budget) {
    static constexpr int k_di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int k_dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    if (m_phase == Phase::walk) {
        // The samples from the end back to the start.
        while (looked < budget) {
            m_cells.push_back({m_wi, m_wj});
            ++looked;
            const u8 from = seen(m_wi, m_wj).from;
            if (from > 7 || m_cells.size() > usize{m_nodes} + 1) { // the start (or, never, a loop)
                std::reverse(m_cells.begin(), m_cells.end());
                m_path.clear();
                m_path.push_back(point(m_cells[0].first, m_cells[0].second));
                m_anchor = 0;
                m_best = 1;
                m_k = 2;
                m_phase = Phase::pull;
                break;
            }
            m_wi -= k_di[from];
            m_wj -= k_dj[from];
        }
        if (m_phase == Phase::walk) return false;
    }
    // Pulled straight: from each corner, as far along as the way is clear
    // (each line looked along costs its length).
    while (m_anchor + 1 < m_cells.size()) {
        if (looked >= budget) return false;
        if (m_k < m_cells.size() && m_k <= m_anchor + k_lookahead) {
            const auto [ai, aj] = m_cells[m_anchor];
            const auto [bi, bj] = m_cells[m_k];
            looked += static_cast<u64>(std::llabs(bi - ai) + std::llabs(bj - aj)) + 1;
            if (clear_line(ai, aj, bi, bj)) {
                m_best = m_k++;
                continue;
            }
        }
        m_path.push_back(point(m_cells[m_best].first, m_cells[m_best].second));
        m_anchor = m_best;
        m_best = m_anchor + 1;
        m_k = m_anchor + 2;
    }
    // Collinear corners left by the lookahead's limit: merged.
    const float step = m_map.settings().step_height;
    std::vector<DVec3> merged;
    for (const DVec3& p : m_path) {
        if (merged.size() >= 2) {
            const DVec3 a = merged[merged.size() - 2], b = merged.back();
            const double cross = (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
            if (std::fabs(cross) < 1e-9 && std::fabs(b.y - a.y) <= step && std::fabs(p.y - b.y) <= step) {
                merged.back() = p;
                continue;
            }
        }
        merged.push_back(p);
    }
    looked += m_path.size();
    m_path = std::move(merged);
    m_cells = {};
    m_blocks.clear();
    m_block_tile.clear();
    m_block_of.clear();
    m_last_block = ~0u;
    m_open = {};
    return true;
}

double PathSearch::length() const {
    double total = 0.0;
    for (usize k = 1; k < m_path.size(); ++k) total += giant::length(m_path[k] - m_path[k - 1]);
    return total;
}

} // namespace giant::nav
