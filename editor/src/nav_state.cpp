#include "nav_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace giant::editor {

namespace {

Vec3 abs3(const Vec3& v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
Vec3 mul3(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

bool same(const nav::Obstacle& a, const nav::Obstacle& b) {
    return a.center == b.center && a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y &&
           a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w && a.half == b.half;
}

bool same(const std::vector<nav::Obstacle>& a, const std::vector<nav::Obstacle>& b) {
    if (a.size() != b.size()) return false;
    for (usize k = 0; k < a.size(); ++k) {
        if (!same(a[k], b[k])) return false;
    }
    return true;
}

// One unit shape (as drawn) scaled by `scale`, placed at `at` turned by
// `rotation`, as a box: false for shapes that stand in nobody's way.
bool box_of(world::Shape shape, const std::string& mesh, const Vec3& scale, const DVec3& at, const Quat& rotation,
            const render::Renderer* renderer, const MeshLibrary* meshes, nav::Obstacle& out) {
    const Vec3 s = abs3(scale);
    out.rotation = rotation;
    switch (shape) {
    case world::Shape::cube:
    case world::Shape::sphere:
        out.center = at;
        out.half = s * 0.5f;
        return true;
    case world::Shape::plane: {
        // A slab half a metre thick whose top is the plane, as the physics has it.
        constexpr float k_half_thickness = 0.25f;
        const Vec3 down = rotate(rotation, Vec3{0.0f, -k_half_thickness, 0.0f});
        out.center = at + DVec3{down.x, down.y, down.z};
        out.half = {0.5f * s.x, k_half_thickness, 0.5f * s.z};
        return true;
    }
    case world::Shape::mesh: {
        // Not loaded (or no renderer): a box of its scale.
        out.center = at;
        out.half = s * 0.5f;
        if (renderer == nullptr || meshes == nullptr) return true;
        const MeshLibrary::Mesh* found = meshes->find_normalized(mesh);
        if (found == nullptr || found->id == render::k_no_mesh) return true;
        const geometry::ClusterMesh* m = renderer->mesh(found->id);
        if (m == nullptr || !(m->bounds.radius > 0.0f) || !std::isfinite(m->bounds.radius)) return true;
        // The box its bounding sphere holds most of (a sphere's box would
        // block far round a house; this one a little less).
        const float side = 0.6f * m->bounds.radius;
        const Vec3 c = rotate(rotation, mul3(m->bounds.center, scale));
        out.center = at + DVec3{c.x, c.y, c.z};
        out.half = s * side;
        return true;
    }
    case world::Shape::kit:
    case world::Shape::decal:
    case world::Shape::cloth: return false;
    }
    return false;
}

constexpr i64 k_ring = 1; // cells around a tile its inputs come from

i64 cell_floor(double metres) { return static_cast<i64>(std::floor(metres / world::k_cell_size)); }

} // namespace

void nav_obstacles(const world::Scene& scene, const world::Entity& e, const render::Renderer* renderer,
                   const MeshLibrary* meshes, std::vector<nav::Obstacle>& out) {
    if (e.body != world::BodyKind::fixed || e.swing.on) return; // (hidden ones collide too)
    if (e.shape == world::Shape::mesh && !e.animation.clip.empty()) return; // a character, moving
    const world::Transform& t = e.transform;
    const Quat rot = normalize(t.rotation);
    if (e.shape != world::Shape::kit) {
        nav::Obstacle o;
        if (box_of(e.shape, e.mesh, t.scale, t.position, rot, renderer, meshes, o)) out.push_back(o);
        return;
    }
    const world::Kit* kit = scene.find_kit(e.kit);
    if (kit == nullptr) return;
    const world::KeptKit* kept = scene.kept_kit(e.id, &t);
    std::vector<u8> loose;
    if (kept != nullptr) {
        loose.assign(kit->pieces.size(), 0);
        for (const world::KeptPiece& p : kept->pieces) {
            if (p.piece < loose.size()) loose[p.piece] = 1;
        }
    }
    for (u32 i = 0; i < kit->pieces.size(); ++i) {
        if (!loose.empty() && loose[i] != 0) continue; // knocked loose: lying elsewhere, as rubble
        const world::KitPiece& p = kit->pieces[i];
        // Where the renderer puts it.
        const Vec3 scaled{static_cast<float>(p.transform.position.x) * t.scale.x,
                          static_cast<float>(p.transform.position.y) * t.scale.y,
                          static_cast<float>(p.transform.position.z) * t.scale.z};
        const Vec3 offset = rotate(rot, scaled);
        nav::Obstacle o;
        if (box_of(p.shape, p.mesh, mul3(t.scale, p.transform.scale), t.position + DVec3{offset.x, offset.y, offset.z},
                   normalize(rot * p.transform.rotation), renderer, meshes, o)) {
            out.push_back(o);
        }
    }
}

NavState::NavState(const render::Renderer* renderer, const MeshLibrary* meshes, const ScatterPlacer* scatter)
    : m_renderer(renderer), m_meshes(meshes), m_scatter(scatter) {}

NavState::~NavState() {
    for (Building& b : m_building) m_jobs.wait(*b.done);
}

void NavState::touch(world::CellCoord cell) {
    m_stamp[cell] = m_next_stamp++;
    if (m_inputs.contains(cell)) m_dirty.insert({cell.x, cell.z});
}

void NavState::unplace(world::EntityId id) {
    const auto it = m_placed.find(id);
    if (it == m_placed.end()) return;
    if (it->second.cells.empty()) {
        m_large.erase(id);
        m_large_stamp = m_next_stamp++;
        for (const auto& [c, in] : m_inputs) m_dirty.insert({c.x, c.z});
    }
    for (const world::CellCoord c : it->second.cells) {
        auto& ids = m_in_cell[c];
        std::erase(ids, id);
        if (ids.empty()) m_in_cell.erase(c);
        touch(c);
    }
    m_placed.erase(it);
}

void NavState::follow_one(const world::Scene& scene, world::EntityId id) {
    const world::Entity* e = scene.find(id);
    Placed now;
    if (e != nullptr) nav_obstacles(scene, *e, m_renderer, m_meshes, now.boxes);
    const auto it = m_placed.find(id);
    if (it != m_placed.end() && same(it->second.boxes, now.boxes)) return; // as it was (moved nothing that matters)
    unplace(id);
    if (now.boxes.empty()) return;
    // The cells its footprints (grown by the agent, and a sample) reach.
    const double grow = settings().agent_radius + nav::k_nav_spacing;
    i64 x0 = std::numeric_limits<i64>::max(), x1 = std::numeric_limits<i64>::min(), z0 = x0, z1 = x1;
    for (const nav::Obstacle& o : now.boxes) {
        const Vec3 a = abs3(rotate(o.rotation, Vec3{o.half.x, 0.0f, 0.0f}));
        const Vec3 b = abs3(rotate(o.rotation, Vec3{0.0f, o.half.y, 0.0f}));
        const Vec3 c = abs3(rotate(o.rotation, Vec3{0.0f, 0.0f, o.half.z}));
        const double rx = a.x + b.x + c.x + grow, rz = a.z + b.z + c.z + grow;
        if (!std::isfinite(rx) || !std::isfinite(rz) || !std::isfinite(o.center.x) || !std::isfinite(o.center.z) ||
            std::fabs(o.center.x) > 1e9 || std::fabs(o.center.z) > 1e9 || rx > 1e9 || rz > 1e9) {
            continue;
        }
        x0 = std::min(x0, cell_floor(o.center.x - rx));
        x1 = std::max(x1, cell_floor(o.center.x + rx));
        z0 = std::min(z0, cell_floor(o.center.z - rz));
        z1 = std::max(z1, cell_floor(o.center.z + rz));
    }
    if (x0 > x1) return; // nowhere
    const i64 count = (x1 - x0 + 1) * (z1 - z0 + 1);
    if (count > static_cast<i64>(k_large_cells) || x0 < std::numeric_limits<i32>::min() || x1 > std::numeric_limits<i32>::max() || z0 < std::numeric_limits<i32>::min() || z1 > std::numeric_limits<i32>::max()) {
        m_large.insert(id);
        m_large_stamp = m_next_stamp++;
        for (const auto& [c, in] : m_inputs) m_dirty.insert({c.x, c.z});
    } else {
        for (i64 z = z0; z <= z1; ++z) {
            for (i64 x = x0; x <= x1; ++x) {
                const world::CellCoord c{static_cast<i32>(x), static_cast<i32>(z)};
                now.cells.push_back(c);
                m_in_cell[c].push_back(id);
                touch(c);
            }
        }
    }
    m_placed[id] = std::move(now);
}

void NavState::follow_all(const world::Scene& scene) {
    std::vector<world::EntityId> ids;
    ids.reserve(m_placed.size() + scene.size());
    for (const auto& [id, p] : m_placed) ids.push_back(id);
    for (const world::Entity& e : scene.entities()) ids.push_back(e.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const world::EntityId id : ids) follow_one(scene, id);
}

void NavState::follow(const world::Scene& scene, const world::SceneChanges& changed) {
    if (scene.identity() != m_scene) {
        // Another scene: what was known of the last one goes, its tiles
        // too (built again as they are asked for; tiles being built are
        // taken stale and built again).
        m_scene = scene.identity();
        m_placed.clear();
        m_in_cell.clear();
        m_large.clear();
        m_large_stamp = m_next_stamp++;
        m_stamp.clear();
        m_map.clear();
        m_inputs.clear();
        m_used.clear();
        m_dirty.clear();
        m_check.clear();
        m_check_next = 0;
        m_refollow.clear();
        m_refollowing.clear();
        m_kits = scene.kits;
        if (m_meshes != nullptr) m_meshes_version = m_meshes->version();
        follow_all(scene);
        return;
    }
    // Kits changed (by name: their pieces differ, or they came or went).
    std::unordered_set<std::string> kits;
    if (!(scene.kits == m_kits)) {
        const auto find = [](const std::vector<world::Kit>& in, const std::string& name) -> const world::Kit* {
            for (const world::Kit& k : in) {
                if (k.name == name) return &k;
            }
            return nullptr;
        };
        for (const world::Kit& k : scene.kits) {
            const world::Kit* was = find(m_kits, k.name);
            if (was == nullptr || !(was->pieces == k.pieces)) kits.insert(k.name);
        }
        for (const world::Kit& k : m_kits) {
            if (find(scene.kits, k.name) == nullptr) kits.insert(k.name);
        }
        m_kits = scene.kits;
    }
    const bool meshes = m_meshes != nullptr && m_meshes->version() != m_meshes_version;
    if (meshes) m_meshes_version = m_meshes->version();
    if (changed.all) {
        follow_all(scene);
        return;
    }
    if (!kits.empty() || meshes) {
        // The entities whose size depends on them: looked at again, a
        // share a frame (update).
        for (const world::Entity& e : scene.entities()) {
            const bool kit = e.shape == world::Shape::kit && (meshes || kits.contains(e.kit));
            if ((kit || (meshes && e.shape == world::Shape::mesh)) && m_refollowing.insert(e.id).second) {
                m_refollow.push_back(e.id);
            }
        }
    }
    for (const world::EntityId id : changed.ids) follow_one(scene, id);
}

NavState::Inputs NavState::inputs_of(const world::Scene& scene, world::CellCoord cell) const {
    Inputs in;
    usize k = 0;
    for (i64 dz = -k_ring; dz <= k_ring; ++dz) {
        for (i64 dx = -k_ring; dx <= k_ring; ++dx, ++k) {
            const world::CellCoord c{static_cast<i32>(cell.x + dx), static_cast<i32>(cell.z + dz)};
            in.terrain[k] = scene.terrain.version(c);
            if (m_scatter != nullptr) {
                const ScatterPlacer::Cell* placed = m_scatter->find(c);
                in.scatter[k] = placed != nullptr ? placed->serial : 0;
            }
        }
    }
    const auto it = m_stamp.find(cell);
    in.entities = it != m_stamp.end() ? it->second : 0;
    in.large = m_large_stamp;
    return in;
}

NavState::Job NavState::job_of(const world::Scene& scene, world::CellCoord cell) const {
    Job job;
    job.terrain.set_samples(scene.terrain.samples());
    for (i64 dz = -k_ring; dz <= k_ring; ++dz) {
        for (i64 dx = -k_ring; dx <= k_ring; ++dx) {
            const world::CellCoord c{static_cast<i32>(cell.x + dx), static_cast<i32>(cell.z + dz)};
            if (const world::Terrain::Heights* h = scene.terrain.find(c)) job.terrain.set(c, *h);
        }
    }
    const auto add = [&](world::EntityId id) {
        const auto p = m_placed.find(id);
        if (p != m_placed.end()) job.obstacles.insert(job.obstacles.end(), p->second.boxes.begin(), p->second.boxes.end());
    };
    if (const auto it = m_in_cell.find(cell); it != m_in_cell.end()) {
        for (const world::EntityId id : it->second) add(id);
    }
    for (const world::EntityId id : m_large) add(id);
    // The scatter trees' trunks around (one reaches over a cell's edge).
    if (m_scatter != nullptr) {
        for (i64 dz = -k_ring; dz <= k_ring; ++dz) {
            for (i64 dx = -k_ring; dx <= k_ring; ++dx) {
                const ScatterPlacer::Cell* placed =
                    m_scatter->find({static_cast<i32>(cell.x + dx), static_cast<i32>(cell.z + dz)});
                if (placed == nullptr || placed->layers == nullptr) continue;
                const ScatterPlacer::Layers& layers = *placed->layers;
                for (const world::ScatterPoint& p : placed->points) {
                    if (p.layer >= layers.size() || p.item >= layers[p.layer].items.size()) continue;
                    const world::ScatterItem& item = layers[p.layer].items[p.item];
                    if (!item.tree) continue;
                    const float scale = std::fabs(p.transform.scale.x); // uniform
                    const float radius = item.tree->trunk_radius * scale;
                    const float height = item.tree->height * scale;
                    if (!(radius > 0.0f) || !(height > 0.0f)) continue;
                    nav::Obstacle o;
                    o.center = p.transform.position + DVec3{0.0, 0.5 * height, 0.0};
                    o.half = {radius, 0.5f * height, radius};
                    job.obstacles.push_back(o);
                }
            }
        }
    }
    return job;
}

void NavState::start(const world::Scene& scene, world::CellCoord cell) {
    Building& b = m_building.emplace_back();
    b.cell = cell;
    b.inputs = inputs_of(scene, cell);
    b.done = std::make_unique<WaitGroup>();
    b.result = std::make_shared<nav::NavTile>();
    auto job = std::make_shared<Job>(job_of(scene, cell));
    m_jobs.submit(*b.done, [cell, job, out = b.result, settings = settings()] {
        *out = nav::build_tile(cell, job->terrain, job->obstacles, settings);
    });
}

void NavState::keep(nav::NavTile tile, const Inputs& inputs) {
    const world::CellCoord c = tile.cell;
    m_inputs[c] = inputs;
    m_used.try_emplace(c, m_frame); // a rebuild is not a use
    m_map.set_tile(std::move(tile));
    ++m_built;
}

void NavState::want(world::CellCoord cell) {
    m_used[cell] = m_frame;
    if (std::find(m_wanted.begin(), m_wanted.end(), cell) == m_wanted.end()) m_wanted.push_back(cell);
}

void NavState::evict() {
    if (m_map.size() <= max_tiles) return;
    // Oldest used first; none used in the last few seconds (a search may be on them).
    constexpr u64 k_recent = 240;
    std::vector<std::pair<u64, world::CellCoord>> old;
    for (const auto& [c, tile] : m_map.tiles()) {
        const auto it = m_used.find(c);
        const u64 used = it != m_used.end() ? it->second : 0;
        if (used + k_recent < m_frame) old.push_back({used, c});
    }
    const usize over = m_map.size() - max_tiles;
    if (old.size() > over) {
        std::nth_element(old.begin(), old.begin() + static_cast<std::ptrdiff_t>(over), old.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        old.resize(over);
    }
    for (const auto& [used, c] : old) {
        m_map.remove_tile(c);
        m_inputs.erase(c);
        m_used.erase(c);
        m_dirty.erase({c.x, c.z});
    }
}

bool NavState::stale_at(const world::Scene& scene, world::CellCoord cell) const {
    const auto in = m_inputs.find(cell);
    return m_map.find(cell) != nullptr && (in == m_inputs.end() || !(in->second == inputs_of(scene, cell)));
}

void NavState::update(const world::Scene& scene) {
    ++m_frame;
    if (scene.identity() == m_scene) {
        for (u32 n = 0; n < refollows_per_frame && !m_refollow.empty(); ++n) {
            const world::EntityId id = m_refollow.front();
            m_refollow.pop_front();
            m_refollowing.erase(id);
            follow_one(scene, id);
        }
    }
    // Tiles finished: kept (a tile whose inputs changed meanwhile is kept
    // for now and built again).
    for (usize k = 0; k < m_building.size();) {
        Building& b = m_building[k];
        if (!b.done->done()) {
            ++k;
            continue;
        }
        const world::CellCoord c = b.cell;
        const bool stale_now = !(inputs_of(scene, c) == b.inputs);
        keep(std::move(*b.result), b.inputs);
        if (stale_now) m_dirty.insert({c.x, c.z});
        m_building.erase(m_building.begin() + static_cast<std::ptrdiff_t>(k));
    }
    // A few built tiles checked against their inputs.
    if (m_check_next >= m_check.size()) {
        m_check.clear();
        m_check_next = 0;
        for (const auto& [c, in] : m_inputs) m_check.push_back(c);
    }
    for (u32 n = 0; n < checks_per_frame && m_check_next < m_check.size(); ++n) {
        const world::CellCoord c = m_check[m_check_next++];
        const auto it = m_inputs.find(c);
        if (it != m_inputs.end() && !(inputs_of(scene, c) == it->second)) m_dirty.insert({c.x, c.z});
    }
    // Tiles started: those searches wait for first, then stale ones.
    const auto building = [&](world::CellCoord c) {
        return std::any_of(m_building.begin(), m_building.end(), [&](const Building& b) { return b.cell == c; });
    };
    while (m_building.size() < tiles_in_flight && !m_wanted.empty()) {
        const world::CellCoord c = m_wanted.front();
        m_wanted.pop_front();
        if (building(c)) continue; // (asked for again if still stale once built)
        if (m_map.find(c) != nullptr && !m_dirty.contains({c.x, c.z})) continue;
        m_dirty.erase({c.x, c.z});
        start(scene, c);
    }
    for (auto it = m_dirty.begin(); m_building.size() < tiles_in_flight && it != m_dirty.end();) {
        const world::CellCoord c{it->first, it->second};
        if (building(c)) {
            ++it;
            continue;
        }
        it = m_dirty.erase(it);
        start(scene, c);
    }
    // Searches moved on, sharing the budget.
    u64 budget = nodes_per_frame;
    for (auto& [id, entry] : m_searches) {
        if (!entry.waits.empty()) {
            const bool fresh = std::none_of(entry.waits.begin(), entry.waits.end(), [&](world::CellCoord c) {
                return m_dirty.contains({c.x, c.z}) ||
                       std::any_of(m_building.begin(), m_building.end(), [&](const Building& b) { return b.cell == c; });
            });
            if (fresh) {
                entry.waits.clear();
                ++entry.redone;
                entry.search = std::make_unique<nav::PathSearch>(m_map, entry.from, entry.to);
            } else {
                for (const world::CellCoord c : entry.waits) want(c);
                continue;
            }
        }
        if (entry.search->state() == nav::SearchState::found || entry.search->state() == nav::SearchState::failed) continue;
        // Its ends' tiles kept (and those it waits for).
        for (const DVec3& end : {entry.from, entry.to}) {
            if (std::fabs(end.x) <= nav::k_max_nav_coordinate && std::fabs(end.z) <= nav::k_max_nav_coordinate) {
                m_used[world::cell_of(end)] = m_frame;
            }
        }
        if (budget == 0) continue;
        nav::PathSearch& search = *entry.search;
        for (int tries = 0; tries < 4 && budget > 0; ++tries) {
            if (search.state() == nav::SearchState::missing && m_map.find(search.missing_tile()) == nullptr) {
                want(search.missing_tile());
                break;
            }
            if (search.state() != nav::SearchState::searching && search.state() != nav::SearchState::missing) break;
            const u64 before = search.work();
            search.advance(static_cast<u32>(budget));
            const u64 spent = std::max<u64>(1, search.work() - before);
            budget = spent >= budget ? 0 : budget - spent;
        }
        if (search.state() != nav::SearchState::found) continue;
        // Found over a tile older than what it is built from (a wall put
        // down there, not built in yet): searched again once it is.
        if (entry.redone >= 4) continue; // as found (the map keeps changing under it)
        const std::vector<DVec3>& path = search.path();
        for (usize k = 0; k + 1 < path.size(); ++k) {
            const double d = std::hypot(path[k + 1].x - path[k].x, path[k + 1].z - path[k].z);
            const int steps = static_cast<int>(std::ceil(d / 8.0)) + 1;
            for (int t = 0; t <= steps; ++t) {
                const DVec3 p = path[k] + (path[k + 1] - path[k]) * (static_cast<double>(t) / steps);
                const world::CellCoord c = world::cell_of(p);
                m_used[c] = m_frame;
                const bool building_here = std::any_of(m_building.begin(), m_building.end(), [&](const Building& b) { return b.cell == c; });
                if ((building_here || m_dirty.contains({c.x, c.z})) &&
                    std::find(entry.waits.begin(), entry.waits.end(), c) == entry.waits.end()) {
                    entry.waits.push_back(c);
                    want(c);
                }
            }
        }
    }
    evict();
}

NavState::SearchId NavState::find_path(const DVec3& from, const DVec3& to) {
    const SearchId id = m_next_search++;
    Search& s = m_searches[id];
    s.search = std::make_unique<nav::PathSearch>(m_map, from, to);
    s.from = from;
    s.to = to;
    return id;
}

const nav::PathSearch* NavState::search(SearchId id) const {
    const auto it = m_searches.find(id);
    return it != m_searches.end() ? it->second.search.get() : nullptr;
}

void NavState::cancel(SearchId id) { m_searches.erase(id); }

const nav::NavTile* NavState::tile_now(const world::Scene& scene, world::CellCoord cell) {
    m_used[cell] = m_frame;
    const nav::NavTile* have = m_map.find(cell);
    const auto in = m_inputs.find(cell);
    const Inputs now = inputs_of(scene, cell);
    if (have != nullptr && in != m_inputs.end() && in->second == now) return have;
    const Job job = job_of(scene, cell);
    keep(nav::build_tile(cell, job.terrain, job.obstacles, settings()), now);
    m_dirty.erase({cell.x, cell.z});
    return m_map.find(cell);
}

std::unique_ptr<nav::PathSearch> NavState::find_path_now(const world::Scene& scene, const DVec3& from, const DVec3& to,
                                                         u32 max_tiles_now, u32* built, bool* stale) {
    u32 made = 0;
    bool stale_left = false;
    const auto stale_tile = [&](world::CellCoord c) { return stale_at(scene, c); };
    const auto cell_at = [](const DVec3& p) {
        return world::CellCoord{static_cast<i32>(std::clamp<i64>(cell_floor(p.x), std::numeric_limits<i32>::min(),
                                                                 std::numeric_limits<i32>::max())),
                                static_cast<i32>(std::clamp<i64>(cell_floor(p.z), std::numeric_limits<i32>::min(),
                                                                 std::numeric_limits<i32>::max()))};
    };
    // The stale tiles at the ends first (and around them), as they are now.
    if (std::isfinite(from.x) && std::isfinite(from.z) && std::isfinite(to.x) && std::isfinite(to.z)) {
        for (const DVec3& end : {from, to}) {
            const world::CellCoord c = cell_at(end);
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const world::CellCoord n{c.x + dx, c.z + dz};
                    if (made < max_tiles_now && stale_tile(n)) {
                        tile_now(scene, n);
                        ++made;
                    }
                }
            }
        }
    }
    std::unique_ptr<nav::PathSearch> search;
    for (;;) {
        search = std::make_unique<nav::PathSearch>(m_map, from, to);
        for (;;) {
            const nav::SearchState state = search->advance(1u << 20);
            if (state != nav::SearchState::missing || made >= max_tiles_now) break;
            tile_now(scene, search->missing_tile());
            ++made;
        }
        if (search->state() != nav::SearchState::found) break;
        // Found over a stale tile (what it was built from has changed since):
        // that tile built again, and searched again.
        bool again = false;
        const std::vector<DVec3>& path = search->path();
        for (usize k = 0; k + 1 < path.size(); ++k) {
            const double d = std::hypot(path[k + 1].x - path[k].x, path[k + 1].z - path[k].z);
            const int steps = static_cast<int>(std::ceil(d / 8.0)) + 1;
            for (int t = 0; t <= steps; ++t) {
                const DVec3 p = path[k] + (path[k + 1] - path[k]) * (static_cast<double>(t) / steps);
                const world::CellCoord c = cell_at(p);
                if (!stale_tile(c)) continue;
                if (made >= max_tiles_now) {
                    stale_left = true;
                    continue;
                }
                tile_now(scene, c);
                ++made;
                again = true;
            }
        }
        if (!again) break;
        stale_left = false;
    }
    if (built != nullptr) *built = made;
    if (stale != nullptr) *stale = stale_left;
    return search;
}

} // namespace giant::editor
