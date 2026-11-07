#include "giant/sim/forest.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_map>

namespace giant::sim {

namespace {

constexpr float k_pi = 3.14159265358979f;

// The trunk's axis: the foot's up.
Vec3 axis_of(const world::Transform& at) {
    const Vec3 up = normalize_or_zero(rotate(at.rotation, Vec3{0.0f, 1.0f, 0.0f}));
    return up == Vec3{} ? Vec3{0.0f, 1.0f, 0.0f} : up;
}

// The trunk along its axis: from `from` to `from + length` metres above
// the foot. Its foot end starts half a radius up (out of the ground on a
// slope, so a falling trunk does not start buried), past what is buried.
struct Span {
    float from = 0.0f;
    float length = 0.0f;
};
Span span_of(const TreeSpot& s) {
    Span out;
    out.from = std::max(0.0f, s.buried) + 0.5f * s.trunk_radius;
    out.length = std::max(s.height - out.from, 2.0f * s.trunk_radius + 0.01f);
    return out;
}

// How fast a felled tree starts tipping (radians a second): a harder blow
// throws it over faster.
float tip_rate(float taken, float strength) {
    return std::clamp(0.4f * taken / std::max(strength, 1.0f), 0.4f, 1.5f);
}

bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool before(world::CellCoord a, world::CellCoord b) { return std::tie(a.x, a.z) < std::tie(b.x, b.z); }

} // namespace

Forest::Forest(PhysicsWorld& physics, const ForestSettings& settings) : m_physics(physics), m_settings(settings) {}

Forest::~Forest() { clear(); }

double Forest::distance(world::CellCoord cell, const DVec3& camera) const {
    const double x = (cell.x + 0.5) * world::k_cell_size - camera.x;
    const double z = (cell.z + 0.5) * world::k_cell_size - camera.z;
    return std::sqrt(x * x + z * z);
}

bool Forest::wants(world::CellCoord cell, const DVec3& camera) const {
    return distance(cell, camera) <= m_settings.radius + world::k_cell_size;
}

u64 Forest::serial(world::CellCoord cell) const {
    const auto it = m_cells.find(cell);
    return it == m_cells.end() ? 0 : it->second.serial;
}

u32 Forest::fallen_in(world::CellCoord cell) const {
    const auto it = m_cells.find(cell);
    return it == m_cells.end() ? 0 : it->second.fallen;
}

std::vector<world::CellCoord> Forest::cells() const {
    std::vector<world::CellCoord> out;
    out.reserve(m_cells.size());
    for (const auto& [coord, cell] : m_cells) out.push_back(coord);
    std::sort(out.begin(), out.end(), before);
    return out;
}

bool Forest::waiting() const {
    if (!m_doomed.empty()) return true; // bodies of trees gone still in the world
    if (m_trunks >= m_settings.max_trunks) return false; // as many as may stand
    for (const auto& [coord, cell] : m_cells) {
        if (cell.next < cell.trees.size() && wants(coord, m_camera)) return true;
    }
    return false;
}

void Forest::redraw(world::CellCoord cell) {
    if (m_redraw_set.insert(cell).second) m_redraw.push_back(cell);
}

BodyDesc Forest::trunk(const TreeSpot& s, Motion motion) const {
    const Span sp = span_of(s);
    const float r = s.trunk_radius;
    BodyDesc d;
    d.parts.push_back({CapsuleShape{0.5f * sp.length - r, r}, Vec3{0.0f, sp.from + 0.5f * sp.length, 0.0f}, Quat{}});
    d.motion = motion;
    d.position = s.at.position;
    d.rotation = s.at.rotation;
    const float volume = k_pi * r * r * (sp.length - 2.0f * r) + (4.0f / 3.0f) * k_pi * r * r * r;
    d.mass = std::max(1.0f, m_settings.density * volume);
    d.friction = 0.9f;
    d.restitution = 0.05f;
    return d;
}

void Forest::drop_bodies(world::CellCoord coord, Cell& cell) {
    std::vector<BodyId> moving; // removed now: a falling body left behind would keep moving undrawn
    for (Tree& t : cell.trees) {
        if (t.body == k_no_body) continue;
        m_by_body.erase(t.body);
        if (m_falling.erase(t.body) > 0) {
            moving.push_back(t.body);
        } else {
            m_doomed.push_back(t.body);
        }
        t.body = k_no_body;
    }
    m_physics.remove_many(moving);
    m_trunks -= cell.trunks;
    cell.trunks = 0;
    cell.next = 0;
    if (cell.fallen > 0) {
        m_fallen_count -= cell.fallen;
        cell.fallen = 0;
        for (Tree& t : cell.trees) {
            if (t.laid) --m_laid;
            t.fallen = false;
            t.laid = false;
        }
        m_fallen.erase(coord);
        redraw(coord);
    }
}

void Forest::drop_trunks(Cell& cell) {
    for (Tree& t : cell.trees) {
        if (t.fallen || t.body == k_no_body) continue;
        m_doomed.push_back(t.body);
        m_by_body.erase(t.body);
        t.body = k_no_body;
    }
    m_trunks -= cell.trunks;
    cell.trunks = 0;
    cell.next = 0;
}

void Forest::set_cell(world::CellCoord coord, u64 serial, std::vector<TreeSpot> trees) {
    // Fallen trees whose spot the new placement keeps (the same point,
    // standing where it stood: the ground under it dented, a cell placed
    // again for other trees) stay fallen, where they lie.
    struct Kept {
        u32 point = 0;
        BodyId body = k_no_body;
        bool laid = false;
        world::Transform at;
    };
    std::vector<Kept> kept;
    if (const auto it = m_cells.find(coord); it != m_cells.end()) {
        Cell& old = it->second;
        const auto lying = m_fallen.find(coord);
        if (old.fallen > 0 && lying != m_fallen.end()) {
            // The new spots by where they stand (to the centimetre): point
            // numbers move when a dent adds or takes another layer's points.
            const auto key = [](const DVec3& p) {
                return (static_cast<u64>(std::llround(p.x * 100.0)) * 0x9E3779B97F4A7C15ull) ^
                       static_cast<u64>(std::llround(p.z * 100.0));
            };
            std::unordered_multimap<u64, usize> by_place;
            by_place.reserve(trees.size());
            for (usize k = 0; k < trees.size(); ++k) by_place.emplace(key(trees[k].at.position), k);
            for (Tree& t : old.trees) {
                if (!t.fallen) continue;
                const auto at = std::lower_bound(lying->second.trees.begin(), lying->second.trees.end(), t.spot.point,
                                                 [](const FallenTree& a, u32 p) { return a.point < p; });
                if (at == lying->second.trees.end() || at->point != t.spot.point) continue;
                // The same tree: where it stood, as big (another model: stands up anew).
                const TreeSpot* same = nullptr;
                const auto [from, to] = by_place.equal_range(key(t.spot.at.position));
                for (auto m = from; m != to; ++m) {
                    const TreeSpot& n = trees[m->second];
                    const DVec3 d = n.at.position - t.spot.at.position;
                    if (std::fabs(d.x) <= 0.01 && std::fabs(d.z) <= 0.01 && n.at.scale == t.spot.at.scale &&
                        n.height == t.spot.height && n.trunk_radius == t.spot.trunk_radius) {
                        same = &n;
                        break;
                    }
                }
                if (same == nullptr) continue;
                world::Transform lies = at->at;
                // With no body (laid, or refused one), it moves with the ground under its foot.
                if (t.body == k_no_body) lies.position.y += same->at.position.y - t.spot.at.position.y;
                kept.push_back({same->point, t.body, t.laid, lies});
                // Out of the old cell, so dropping it leaves this one be.
                if (t.body != k_no_body) m_by_body.erase(t.body);
                t.body = k_no_body;
            }
        }
        drop_bodies(coord, old); // (the kept ones' bodies are not the old cell's now: left as they are)
    }
    Cell& cell = m_cells[coord];
    cell = Cell{};
    cell.serial = serial;
    cell.trees.reserve(trees.size());
    for (TreeSpot& s : trees) {
        Tree& t = cell.trees.emplace_back();
        t.spot = s;
        cell.reach = std::max(cell.reach, s.height + s.trunk_radius);
    }
    if (kept.empty()) return;
    std::sort(kept.begin(), kept.end(), [](const Kept& x, const Kept& y) { return x.point < y.point; });
    FallenCell& f = m_fallen[coord];
    f.serial = serial;
    f.trees.clear();
    usize k = 0;
    for (const Kept& kp : kept) {
        while (k < cell.trees.size() && cell.trees[k].spot.point < kp.point) ++k;
        if (k == cell.trees.size() || cell.trees[k].spot.point != kp.point || cell.trees[k].fallen) {
            if (kp.body != k_no_body) { // (two kept on one spot: the second goes)
                m_falling.erase(kp.body);
                m_doomed.push_back(kp.body);
            }
            continue;
        }
        Tree& t = cell.trees[k];
        t.fallen = true;
        t.laid = kp.laid;
        t.body = kp.body;
        if (kp.body != k_no_body) m_by_body[kp.body] = {coord, static_cast<u32>(k)};
        ++cell.fallen;
        ++m_fallen_count;
        if (kp.laid) ++m_laid;
        f.trees.push_back({kp.point, kp.at});
    }
    redraw(coord);
}

void Forest::remove_cell(world::CellCoord coord) {
    const auto it = m_cells.find(coord);
    if (it == m_cells.end()) return;
    drop_bodies(coord, it->second);
    m_cells.erase(it);
}

void Forest::sync(const DVec3& camera) { sync(camera, m_settings.trunks_per_sync); }

void Forest::flush(const DVec3& camera) { sync(camera, std::numeric_limits<u32>::max()); }

void Forest::sync(const DVec3& camera, u32 budget) {
    m_camera = camera;
    // Bodies no tree has now go first (within the budget).
    if (!m_doomed.empty()) {
        const usize n = std::min<usize>(m_doomed.size(), budget);
        m_physics.remove_many(std::span<const BodyId>(m_doomed.data() + m_doomed.size() - n, n));
        m_doomed.resize(m_doomed.size() - n);
        budget -= static_cast<u32>(n);
    }
    std::vector<std::pair<double, world::CellCoord>> near;
    std::vector<std::pair<double, world::CellCoord>> holding; // cells with trunks, by distance
    std::vector<world::CellCoord> forget;
    const double reach = m_settings.radius + world::k_cell_size;
    for (auto& [coord, cell] : m_cells) {
        const double d = distance(coord, camera);
        if (d > reach + world::k_cell_size) {
            // Far: its standing trunks go (fallen ones stay where they lie).
            drop_trunks(cell);
            if (cell.trunks == 0 && cell.fallen == 0) forget.push_back(coord);
            continue;
        }
        if (cell.trunks > 0) holding.push_back({d, coord});
        if (d <= reach && cell.next < cell.trees.size()) near.push_back({d, coord});
    }
    for (const world::CellCoord c : forget) m_cells.erase(c);
    const auto by_distance = [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : before(a.second, b.second);
    };
    std::sort(near.begin(), near.end(), by_distance);
    std::sort(holding.begin(), holding.end(), by_distance);
    std::vector<BodyDesc> descs;
    std::vector<u32> which;
    for (const auto& [d, coord] : near) {
        if (budget == 0) break;
        // Full: the farthest cell's trunks make room, if it is farther.
        while (m_trunks >= m_settings.max_trunks && !holding.empty() && holding.back().first > d) {
            drop_trunks(m_cells.at(holding.back().second));
            holding.pop_back();
        }
        Cell& cell = m_cells.at(coord);
        descs.clear();
        which.clear();
        while (cell.next < cell.trees.size() && budget > 0 && m_trunks + descs.size() < m_settings.max_trunks) {
            const u32 i = static_cast<u32>(cell.next++);
            const Tree& t = cell.trees[i];
            if (t.fallen || t.body != k_no_body) continue;
            descs.push_back(trunk(t.spot, Motion::fixed));
            which.push_back(i);
            --budget;
        }
        const std::vector<BodyId> ids = m_physics.add_many(descs);
        for (usize k = 0; k < ids.size(); ++k) {
            if (ids[k] == k_no_body) {
                ++m_refused;
                continue;
            }
            cell.trees[which[k]].body = ids[k];
            m_by_body[ids[k]] = {coord, which[k]};
            ++cell.trunks;
            ++m_trunks;
        }
        if (m_trunks >= m_settings.max_trunks && (holding.empty() || holding.back().first <= d)) break;
    }
}

Vec3 Forest::across_of(const world::Transform& at, const Vec3& away) {
    const Vec3 axis = axis_of(at);
    Vec3 across = normalize_or_zero(away - axis * dot(away, axis));
    if (across == Vec3{}) {
        across = normalize_or_zero(cross(axis, Vec3{0.0f, 0.0f, 1.0f}));
        if (across == Vec3{}) across = Vec3{1.0f, 0.0f, 0.0f};
    }
    return across;
}

usize Forest::topple(std::vector<Topple>& topples) {
    // A tree struck twice: by the hardest blow. Then the hardest blows first.
    std::sort(topples.begin(), topples.end(), [](const Topple& a, const Topple& b) {
        return std::tie(a.cell.x, a.cell.z, a.index, b.taken) < std::tie(b.cell.x, b.cell.z, b.index, a.taken);
    });
    topples.erase(std::unique(topples.begin(), topples.end(),
                              [](const Topple& a, const Topple& b) { return a.cell == b.cell && a.index == b.index; }),
                  topples.end());
    std::stable_sort(topples.begin(), topples.end(), [](const Topple& a, const Topple& b) { return a.taken > b.taken; });
    std::vector<BodyDesc> descs;
    std::vector<const Topple*> felled;
    std::vector<BodyId> gone;
    for (const Topple& t : topples) {
        Cell& cell = m_cells.at(t.cell);
        Tree& tree = cell.trees[t.index];
        if (tree.fallen || tree.body == k_no_body) continue;
        if (descs.size() >= m_settings.topples_per_step || m_falling.size() + descs.size() >= m_settings.max_falling ||
            m_fallen_count + descs.size() >= m_settings.max_fallen) {
            ++m_shrugged;
            continue;
        }
        const Vec3 axis = axis_of(tree.spot.at);
        const Vec3 across = across_of(tree.spot.at, t.away);
        const float rate = tip_rate(t.taken, tree.spot.strength);
        const Span sp = span_of(tree.spot);
        BodyDesc d = trunk(tree.spot, Motion::dynamic);
        // Turning about its foot: the axis tips toward `across`, its
        // middle moving that way.
        d.angular_velocity = cross(axis, across) * rate;
        d.linear_velocity = across * (rate * (sp.from + 0.5f * sp.length));
        descs.push_back(d);
        felled.push_back(&t);
        gone.push_back(tree.body);
        m_by_body.erase(tree.body);
        tree.body = k_no_body;
    }
    // The standing trunks go first: a body made inside a fixed one is thrown out of it.
    m_physics.remove_many(gone);
    const std::vector<BodyId> ids = m_physics.add_many(descs);
    for (usize k = 0; k < felled.size(); ++k) {
        const Topple& t = *felled[k];
        Cell& cell = m_cells.at(t.cell);
        Tree& tree = cell.trees[t.index];
        tree.fallen = true;
        tree.body = ids[k];
        --cell.trunks;
        --m_trunks;
        ++cell.fallen;
        ++m_fallen_count;
        if (ids[k] == k_no_body) {
            ++m_refused; // gone, drawn where it stood
        } else {
            m_by_body[ids[k]] = {t.cell, t.index};
            m_falling[ids[k]] = {};
        }
        FallenCell& f = m_fallen[t.cell];
        f.serial = cell.serial;
        const FallenTree entry{tree.spot.point, tree.spot.at};
        const auto at = std::lower_bound(f.trees.begin(), f.trees.end(), entry.point,
                                         [](const FallenTree& a, u32 p) { return a.point < p; });
        f.trees.insert(at, entry);
        redraw(t.cell);
        if (m_dust != nullptr) {
            // Torn up at the foot.
            const float r = std::clamp(0.2f * tree.spot.height, 1.5f, 6.0f);
            m_dust->emit(tree.spot.at.position + DVec3{0.0, 0.5 * r, 0.0}, r, 0.6f / r, k_dust_earth);
        }
    }
    return felled.size();
}

usize Forest::lay(std::vector<Topple>& lays) {
    std::sort(lays.begin(), lays.end(), [](const Topple& a, const Topple& b) {
        return a.taken != b.taken ? a.taken > b.taken : std::tie(a.cell.x, a.cell.z, a.index) < std::tie(b.cell.x, b.cell.z, b.index);
    });
    usize laid = 0;
    for (const Topple& t : lays) {
        Cell& cell = m_cells.at(t.cell);
        Tree& tree = cell.trees[t.index];
        if (tree.fallen || tree.body != k_no_body) continue;
        if (laid >= m_settings.lays_per_impact || m_fallen_count >= m_settings.max_fallen) {
            ++m_shrugged;
            continue;
        }
        const world::Transform& foot = tree.spot.at;
        const Vec3 axis = axis_of(foot);
        const Vec3 across = across_of(foot, t.away);
        const float h = std::max(tree.spot.height, 0.1f);
        // Tipped about its foot until its top is on what is under it there
        // (past standing trunks: they would be struck too), lying no
        // flatter than a little past level and no steeper than half up.
        float tip = 0.5f * k_pi;
        const Vec3 over = across * (0.95f * h) + Vec3{0.0f, h, 0.0f};
        const DVec3 from = foot.position + DVec3{over.x, over.y, over.z};
        // Fixed bodies only (what falls would leave it in the air), past standing trunks.
        PhysicsWorld::RayFilter filter;
        filter.fixed_only = true;
        filter.accept = [this](BodyId body, u64) { return !m_by_body.contains(body); };
        if (const std::optional<RayHit> hit = m_physics.raycast(from, {0.0f, -1.0f, 0.0f}, 2.0 * h + 50.0, filter)) {
            const DVec3 d = hit->point - foot.position;
            const Vec3 v = Vec3{static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)} +
                           Vec3{0.0f, tree.spot.trunk_radius, 0.0f};
            const float a = dot(v, axis);
            const float c = dot(v, across);
            if (std::isfinite(a) && std::isfinite(c) && (a != 0.0f || c != 0.0f)) {
                tip = std::clamp(std::atan2(c, a), 0.25f * k_pi, 0.6f * k_pi);
            }
        }
        Vec3 turn = normalize_or_zero(cross(axis, across));
        if (turn == Vec3{}) turn = Vec3{0.0f, 0.0f, 1.0f};
        tree.fallen = true;
        tree.laid = true;
        ++cell.fallen;
        ++m_fallen_count;
        ++m_laid;
        ++laid;
        FallenCell& f = m_fallen[t.cell];
        f.serial = cell.serial;
        world::Transform lying = foot;
        lying.rotation = normalize(quat_from_axis_angle(turn, tip) * foot.rotation);
        const FallenTree entry{tree.spot.point, lying};
        const auto at = std::lower_bound(f.trees.begin(), f.trees.end(), entry.point,
                                         [](const FallenTree& x, u32 p) { return x.point < p; });
        f.trees.insert(at, entry);
        redraw(t.cell);
        if (m_dust != nullptr) {
            // Where it came down: along its middle.
            const Vec3 mid = rotate(lying.rotation, Vec3{0.0f, 0.5f * h, 0.0f});
            const float r = std::clamp(0.3f * h, 2.0f, 10.0f);
            m_dust->emit(foot.position + DVec3{mid.x, mid.y + 0.3 * r, mid.z}, r, 0.5f / r, k_dust_earth);
        }
    }
    return laid;
}

void Forest::settle() {
    // Falling trees come to rest: asleep, they cost the steps nothing (and
    // wake, falling again, when what they rest on goes).
    std::vector<BodyId> still;
    for (const auto& [body, f] : m_falling) {
        const std::optional<BodyState> s = m_physics.state(body);
        if (!s || !s->active) still.push_back(body);
    }
    for (const BodyId body : still) m_falling.erase(body);
}

usize Forest::after_step() {
    std::vector<Topple> topples;
    const auto struck = [&](BodyId body, float impulse, const Vec3& away) {
        const auto owner = m_by_body.find(body);
        if (owner == m_by_body.end()) return;
        const Tree& t = m_cells.at(owner->second.cell).trees[owner->second.index];
        if (t.fallen || !(impulse >= t.spot.strength)) return;
        topples.push_back({owner->second.cell, owner->second.index, away, impulse});
    };
    // A falling tree landing hard throws up dust where it strikes.
    const auto landed = [&](BodyId body, float impulse, const DVec3& point) {
        if (m_dust == nullptr || !m_falling.contains(body)) return;
        const auto owner = m_by_body.find(body);
        if (owner == m_by_body.end()) return;
        const Tree& t = m_cells.at(owner->second.cell).trees[owner->second.index];
        if (!(impulse >= 0.25f * t.spot.strength)) return;
        const float r = std::clamp(0.25f * t.spot.height, 2.0f, 8.0f);
        m_dust->emit(point + DVec3{0.0, 0.3 * r, 0.0}, r, 0.5f / r, k_dust_earth);
    };
    for (const ContactHit& h : m_physics.hits()) {
        struck(h.b, h.impulse, h.normal);         // pushed along the normal (a to b)
        struck(h.a, h.impulse, h.normal * -1.0f); // and a the other way
        landed(h.a, h.impulse, h.point);
        landed(h.b, h.impulse, h.point);
    }
    const usize felled = topples.empty() ? 0 : topple(topples);
    // Where the moving ones are now (resting ones woken too: falling again).
    for (const BodyId body : m_physics.moved()) {
        const auto owner = m_by_body.find(body);
        if (owner == m_by_body.end()) continue;
        const Tree& t = m_cells.at(owner->second.cell).trees[owner->second.index];
        if (!t.fallen) continue;
        m_falling.try_emplace(body);
        const std::optional<BodyState> s = m_physics.state(body);
        if (!s) continue;
        FallenCell& fc = m_fallen[owner->second.cell];
        const auto at = std::lower_bound(fc.trees.begin(), fc.trees.end(), t.spot.point,
                                         [](const FallenTree& a, u32 p) { return a.point < p; });
        if (at == fc.trees.end() || at->point != t.spot.point) continue;
        const world::Transform now{s->position, s->rotation, t.spot.at.scale};
        if (now == at->at) continue;
        at->at = now;
        redraw(owner->second.cell);
    }
    settle();
    return felled;
}

usize Forest::impact(const DVec3& point, float radius, float impulse) {
    if (!std::isfinite(radius) || radius <= 0.0f || !std::isfinite(impulse) || impulse <= 0.0f || !finite(point)) {
        return 0;
    }
    std::vector<Topple> topples;
    std::vector<Topple> lays;
    for (auto& [coord, cell] : m_cells) {
        if (cell.fallen >= cell.trees.size()) continue;
        // The cell's square on the ground, widened by its tallest tree.
        const double x0 = coord.x * world::k_cell_size, z0 = coord.z * world::k_cell_size;
        const double dx = std::max({x0 - point.x, 0.0, point.x - (x0 + world::k_cell_size)});
        const double dz = std::max({z0 - point.z, 0.0, point.z - (z0 + world::k_cell_size)});
        if (std::sqrt(dx * dx + dz * dz) > static_cast<double>(radius) + cell.reach) continue;
        for (u32 i = 0; i < cell.trees.size(); ++i) {
            const Tree& t = cell.trees[i];
            if (t.fallen) continue;
            // The nearest point of its trunk (foot to top) to the blow.
            const DVec3 rel_d = point - t.spot.at.position;
            const Vec3 rel{static_cast<float>(rel_d.x), static_cast<float>(rel_d.y), static_cast<float>(rel_d.z)};
            const Vec3 axis = axis_of(t.spot.at);
            const float along = std::clamp(dot(rel, axis), 0.0f, t.spot.height);
            const Vec3 to_trunk = axis * along - rel; // from the blow to the trunk
            const float gap = std::max(0.0f, length(to_trunk) - t.spot.trunk_radius);
            if (gap > radius) continue;
            const float taken = impulse * (1.0f - gap / radius);
            if (taken < t.spot.strength) continue;
            (t.body == k_no_body ? lays : topples).push_back({coord, i, to_trunk, taken});
        }
    }
    usize felled = topples.empty() ? 0 : topple(topples);
    felled += lays.empty() ? 0 : lay(lays);
    return felled;
}

void Forest::reset() {
    std::vector<BodyId> moving;
    for (auto& [coord, cell] : m_cells) {
        if (cell.fallen == 0) continue;
        for (Tree& t : cell.trees) {
            if (!t.fallen) continue;
            if (t.body != k_no_body) {
                m_by_body.erase(t.body);
                // Moving ones go now (a trunk made where one moves would
                // throw it); resting ones sleep, and go within the budget.
                const std::optional<BodyState> st = m_physics.state(t.body);
                (m_falling.contains(t.body) || (st && st->active) ? moving : m_doomed).push_back(t.body);
                t.body = k_no_body;
            }
            t.fallen = false;
            t.laid = false;
        }
        cell.fallen = 0;
        cell.next = 0; // trunks made again where they stood
    }
    m_falling.clear();
    for (const auto& [coord, f] : m_fallen) redraw(coord);
    m_fallen.clear();
    m_fallen_count = 0;
    m_laid = 0;
    std::sort(moving.begin(), moving.end());
    m_physics.remove_many(moving);
}

usize Forest::forget() {
    const usize bodies = m_by_body.size() + m_doomed.size();
    for (const auto& [coord, f] : m_fallen) redraw(coord);
    m_fallen.clear();
    m_cells.clear();
    m_by_body.clear();
    m_falling.clear();
    m_doomed.clear();
    m_trunks = 0;
    m_fallen_count = 0;
    m_laid = 0;
    return bodies;
}

void Forest::clear() {
    std::vector<BodyId> gone = m_doomed;
    gone.reserve(m_by_body.size() + m_doomed.size());
    for (const auto& [body, owner] : m_by_body) gone.push_back(body);
    std::sort(gone.begin(), gone.end()); // the same order every run
    m_physics.remove_many(gone, false);
    forget();
}

std::vector<world::CellCoord> Forest::take_redraw() {
    std::vector<world::CellCoord> out;
    out.swap(m_redraw);
    m_redraw_set.clear();
    std::sort(out.begin(), out.end(), before);
    return out;
}

std::optional<Forest::TreeRef> Forest::tree_of(BodyId body) const {
    const auto owner = m_by_body.find(body);
    if (owner == m_by_body.end()) return std::nullopt;
    const Tree& t = m_cells.at(owner->second.cell).trees[owner->second.index];
    return TreeRef{owner->second.cell, t.spot.point, t.fallen};
}

BodyId Forest::body_of(world::CellCoord coord, u32 point) const {
    const auto it = m_cells.find(coord);
    if (it == m_cells.end()) return k_no_body;
    const std::vector<Tree>& trees = it->second.trees;
    const auto t = std::lower_bound(trees.begin(), trees.end(), point,
                                    [](const Tree& a, u32 p) { return a.spot.point < p; });
    return t == trees.end() || t->spot.point != point ? k_no_body : t->body;
}

ForestStats Forest::stats() const {
    ForestStats s;
    s.cells = static_cast<u32>(m_cells.size());
    for (const auto& [coord, cell] : m_cells) {
        s.trees += cell.trees.size();
        if (wants(coord, m_camera)) {
            for (usize i = cell.next; i < cell.trees.size(); ++i) {
                if (!cell.trees[i].fallen && cell.trees[i].body == k_no_body) ++s.waiting;
            }
        }
    }
    s.trunks = m_trunks;
    s.falling = static_cast<u32>(m_falling.size());
    s.fallen = m_fallen_count;
    s.refused = m_refused;
    s.shrugged = m_shrugged;
    s.laid = m_laid;
    return s;
}

} // namespace giant::sim
