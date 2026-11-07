// ScenePhysics: entities and ground tiles as bodies, kept in step.
#include "giant/sim/scene_physics.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <cmath>
#include <limits>

namespace giant::sim {

namespace {

// Shape, kit, size and body (and mass, for a dynamic one): what a body is
// made from (not where it is, what it is called or how it looks).
bool same_body(const world::Entity& a, const world::Entity& b) {
    return a.shape == b.shape && a.kit == b.kit && a.transform.scale == b.transform.scale && a.body == b.body &&
           (a.body != world::BodyKind::dynamic || a.mass == b.mass);
}

// The kits whose pieces differ (or that came or went), by name.
std::vector<std::string> changed_kits(const std::vector<world::Kit>& was, const std::vector<world::Kit>& now) {
    std::vector<std::string> out;
    const auto find = [](const std::vector<world::Kit>& kits, const std::string& name) -> const world::Kit* {
        for (const world::Kit& k : kits) {
            if (k.name == name) return &k;
        }
        return nullptr;
    };
    for (const world::Kit& k : now) {
        const world::Kit* before = find(was, k.name);
        if (before == nullptr || !(before->pieces == k.pieces)) out.push_back(k.name);
    }
    for (const world::Kit& k : was) {
        if (find(now, k.name) == nullptr) out.push_back(k.name);
    }
    return out;
}

Vec3 abs3(const Vec3& v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
Vec3 mul3(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

// A unit shape (cube, sphere, plane as the renderer draws them) scaled by
// `scale`, as a part placed at `offset`, turned by `rotation`. False: no
// collision for that shape.
bool add_part(world::Shape shape, const Vec3& scale, const Vec3& offset, const Quat& rotation,
              std::vector<Part>& parts) {
    const Vec3 s = abs3(scale);
    switch (shape) {
    case world::Shape::cube:
        parts.push_back({BoxShape{{0.5f * s.x, 0.5f * s.y, 0.5f * s.z}}, offset, rotation});
        return true;
    case world::Shape::sphere: {
        const float lo = std::min({s.x, s.y, s.z});
        const float hi = std::max({s.x, s.y, s.z});
        if (hi - lo <= 1e-4f * hi) {
            parts.push_back({SphereShape{0.5f * hi}, offset, rotation});
            return true;
        }
        // Unevenly scaled: an ellipsoid, as a hull of points round it.
        ConvexShape hull;
        constexpr int k_rings = 6;
        constexpr int k_around = 10;
        hull.points.push_back({0.0f, 0.5f * s.y, 0.0f});
        hull.points.push_back({0.0f, -0.5f * s.y, 0.0f});
        for (int r = 1; r < k_rings; ++r) {
            const float lat = 3.14159265f * (static_cast<float>(r) / k_rings - 0.5f);
            for (int a = 0; a < k_around; ++a) {
                const float lon = 6.28318531f * static_cast<float>(a) / k_around;
                hull.points.push_back({0.5f * s.x * std::cos(lat) * std::cos(lon), 0.5f * s.y * std::sin(lat),
                                       0.5f * s.z * std::cos(lat) * std::sin(lon)});
            }
        }
        parts.push_back({std::move(hull), offset, rotation});
        return true;
    }
    case world::Shape::plane: {
        // A slab half a metre thick whose top is the plane (y = 0 in its frame).
        constexpr float k_half_thickness = 0.25f;
        const Vec3 down = rotate(rotation, Vec3{0.0f, -k_half_thickness, 0.0f});
        parts.push_back({BoxShape{{0.5f * s.x, k_half_thickness, 0.5f * s.z}},
                         {offset.x + down.x, offset.y + down.y, offset.z + down.z},
                         rotation});
        return true;
    }
    case world::Shape::mesh:
    case world::Shape::kit:
    case world::Shape::decal:
        return false;
    }
    return false;
}

} // namespace

// A tile's shape, made on the builder's worker.
struct ScenePhysics::Build {
    u64 versions[4] = {};
    BodyDesc desc;          // where the tile goes (its height samples are moved into the shape)
    PreparedShape shape;    // set by the job
    std::atomic<bool> done{false};
    std::atomic<bool> superseded{false}; // a newer build (or none) took its place: skip the work
};

ScenePhysics::ScenePhysics(PhysicsWorld& physics, const ScenePhysicsSettings& settings)
    : m_physics(physics), m_settings(settings) {}

ScenePhysics::~ScenePhysics() {
    m_builder.wait(m_builds);
    m_closing = true;
    clear();
}

void ScenePhysics::clear() {
    std::vector<BodyId> gone;
    gone.reserve(m_entities.size() + m_tiles.size());
    for (const auto& [id, t] : m_entities) {
        if (t.body != k_no_body) gone.push_back(t.body);
    }
    for (const auto& [cell, tile] : m_tiles) {
        if (tile.body != k_no_body) gone.push_back(tile.body);
        if (tile.building != nullptr) tile.building->superseded.store(true, std::memory_order_relaxed);
    }
    if (!m_closing && !gone.empty() && gone.size() == m_physics.size()) {
        m_physics.clear(); // all of them ours: a fresh world, the old let go off the frame
    } else {
        m_physics.remove_many(gone, false); // all of ours: nothing left to wake
    }
    for (const auto& [id, pose] : m_poses) m_redraw.push_back(id);
    // Freeing tens of thousands of entries takes milliseconds: on the
    // builder's worker, off the frame.
    struct Garbage {
        std::unordered_map<world::EntityId, Tracked> entities;
        std::unordered_map<BodyId, world::EntityId> by_body;
        std::unordered_map<world::CellCoord, Tile, world::CellCoordHash> tiles;
        std::unordered_set<world::EntityId> queued;
    };
    auto garbage = std::make_unique<Garbage>(
        Garbage{std::move(m_entities), std::move(m_by_body), std::move(m_tiles), std::move(m_queued)});
    m_builder.submit(m_builds, [g = std::shared_ptr<Garbage>(std::move(garbage))]() mutable { g.reset(); });
    m_entities = {};
    m_by_body = {};
    m_poses.clear();
    m_queue.clear();
    m_dynamic_queue.clear();
    m_dynamic_queued.clear();
    m_edited.clear();
    m_queued = {};
    m_scan.clear();
    m_scan_at = 0;
    m_tiles = {}; // builds on their way finish into nothing
    m_tile_queue.clear();
    m_tile_queued.clear();
    m_building.clear();
    m_kits.clear();
    m_scene = 0;
}

void ScenePhysics::drop_pose(world::EntityId id) {
    if (m_poses.erase(id) != 0) m_redraw.push_back(id);
}

BodyDesc ScenePhysics::entity_body(const world::Scene& scene, const world::Entity& e) const {
    BodyDesc d;
    if (e.body == world::BodyKind::none) return d;
    d.position = e.transform.position;
    d.rotation = e.transform.rotation;
    d.user = e.id;
    if (e.shape == world::Shape::kit) {
        const world::Kit* kit = scene.find_kit(e.kit);
        if (kit == nullptr) return d;
        const Vec3& ps = e.transform.scale;
        for (const world::KitPiece& p : kit->pieces) {
            // As the renderer places a piece: its offset scaled by the kit's scale.
            const Vec3 offset{static_cast<float>(p.transform.position.x) * ps.x,
                              static_cast<float>(p.transform.position.y) * ps.y,
                              static_cast<float>(p.transform.position.z) * ps.z};
            add_part(p.shape, mul3(p.transform.scale, ps), offset, p.transform.rotation, d.parts);
        }
        d.motion = Motion::fixed; // kits break from N4 part 4; they never fall whole
        return d;
    }
    add_part(e.shape, e.transform.scale, {}, {}, d.parts);
    if (e.body == world::BodyKind::dynamic) {
        d.motion = Motion::dynamic;
        d.mass = e.mass;
    }
    return d;
}

void ScenePhysics::enqueue(const world::Scene& scene, world::EntityId id) {
    // Moved by an edit: drawn where it was put now, though its body may wait.
    if (m_poses.count(id) != 0) {
        const world::Entity* e = scene.find(id);
        const auto it = m_entities.find(id);
        if (e == nullptr || it == m_entities.end() || !(e->transform.position == it->second.look.transform.position) ||
            !(e->transform.rotation == it->second.look.transform.rotation)) {
            drop_pose(id);
            m_edited.insert(id);
        }
    }
    // A dynamic one (now, or as its body was made) goes to its own queue,
    // with its share of each sync: an edit to it never waits behind a
    // stream of fixed ones.
    const world::Entity* e = scene.find(id);
    const auto it = m_entities.find(id);
    const bool dynamic = (e != nullptr && e->body == world::BodyKind::dynamic && e->shape != world::Shape::kit) ||
                         (it != m_entities.end() && it->second.dynamic);
    if (dynamic) {
        if (m_dynamic_queued.insert(id).second) m_dynamic_queue.push_back(id);
    } else if (m_queued.insert(id).second) {
        m_queue.push_back(id);
    }
}

void ScenePhysics::enqueue_tile(world::CellCoord cell) {
    if (m_tile_queued.insert(cell).second) m_tile_queue.push_back(cell);
}

u32 ScenePhysics::sync_entity(const world::Scene& scene, world::EntityId id, std::vector<Made>& made,
                              std::vector<BodyId>& gone) {
    m_edited.erase(id);
    const world::Entity* e = scene.find(id);
    const auto it = m_entities.find(id);
    if (e == nullptr) {
        if (it != m_entities.end()) {
            if (it->second.body != k_no_body) {
                gone.push_back(it->second.body);
                m_by_body.erase(it->second.body);
            }
            m_entities.erase(it);
        }
        m_poses.erase(id); // gone from the scene: nothing to draw
        return 1;
    }
    if (it != m_entities.end() && !it->second.remake && same_body(it->second.look, *e)) {
        Tracked& t = it->second;
        if (t.look.transform.position == e->transform.position && t.look.transform.rotation == e->transform.rotation) {
            return 1; // unchanged where it counts (a colour, a name)
        }
        // Moved by an edit: put there, at rest, and drawn there.
        if (t.body != k_no_body) {
            m_physics.set_transform(t.body, e->transform.position, e->transform.rotation);
            if (t.dynamic) m_physics.set_velocity(t.body, {}, {});
        }
        t.look.transform = e->transform;
        drop_pose(id);
        return 1;
    }
    if (it != m_entities.end() && it->second.body != k_no_body) {
        gone.push_back(it->second.body);
        m_by_body.erase(it->second.body);
    }
    drop_pose(id);
    Tracked& t = m_entities[id];
    t = Tracked{};
    t.look = *e;
    BodyDesc desc = entity_body(scene, *e);
    const u32 cost = std::max<u32>(1, static_cast<u32>(desc.parts.size())); // a kit's pieces count each
    if (!desc.parts.empty()) {
        const bool dynamic = desc.motion == Motion::dynamic;
        made.push_back({id, std::move(desc), dynamic});
    }
    return cost;
}

bool ScenePhysics::busy() const {
    return !m_queue.empty() || !m_dynamic_queue.empty() || m_scan_at < m_scan.size() || !m_tile_queued.empty() ||
           !m_building.empty();
}

bool ScenePhysics::ground_waiting(const world::Terrain& terrain, const DVec3& at) const {
    if (m_tile_queued.empty() && m_building.empty()) return false;
    const world::CellCoord c = terrain.tile_of(at.x, at.z);
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const world::CellCoord n{c.x + dx, c.z + dz};
            if (m_tile_queued.count(n) != 0) return true;
            const auto it = m_tiles.find(n);
            if (it != m_tiles.end() && it->second.building != nullptr && it->second.body == k_no_body) return true;
        }
    }
    return false;
}

void ScenePhysics::sync_entities(const world::Scene& scene, u32 budget) {
    std::vector<Made> made;
    std::vector<BodyId> gone;
    // A new dynamic body waits for what it could land on: the fixed bodies
    // waiting, and the ground round it.
    const auto is_new_dynamic = [&](world::EntityId id, const world::Entity* e) {
        if (e == nullptr || e->body != world::BodyKind::dynamic || e->shape == world::Shape::kit) return false;
        const auto it = m_entities.find(id);
        return it == m_entities.end() || !it->second.dynamic;
    };
    // An eighth kept for dynamic ones (an edit to one must not wait on a
    // stream of fixed ones).
    const u32 reserve = budget / 8;
    u32 fixed_budget = budget - reserve;
    const auto next_fixed = [&](world::EntityId& id) {
        if (!m_queue.empty()) {
            id = m_queue.front();
            m_queue.pop_front();
            m_queued.erase(id);
            return true;
        }
        if (m_scan_at < m_scan.size()) {
            id = m_scan[m_scan_at++];
            if (m_scan_at == m_scan.size()) {
                m_scan.clear();
                m_scan_at = 0;
            }
            return true;
        }
        return false;
    };
    world::EntityId id = 0;
    while (fixed_budget > 0 && next_fixed(id)) {
        const world::Entity* e = scene.find(id);
        if (is_new_dynamic(id, e)) {
            if (m_dynamic_queued.insert(id).second) m_dynamic_queue.push_back(id);
            --fixed_budget;
            continue;
        }
        const u32 cost = sync_entity(scene, id, made, gone);
        fixed_budget -= std::min(cost, fixed_budget); // a big kit may overrun it, never the reserve
    }
    budget = reserve + fixed_budget;
    const bool fixed_waiting = !m_queue.empty() || m_scan_at < m_scan.size();
    usize looked = 0;
    const usize waiting = m_dynamic_queue.size();
    while (budget > 0 && looked < waiting && !m_dynamic_queue.empty()) {
        ++looked;
        id = m_dynamic_queue.front();
        m_dynamic_queue.pop_front();
        const world::Entity* e = scene.find(id);
        if (is_new_dynamic(id, e) && (fixed_waiting || ground_waiting(scene.terrain, e->transform.position))) {
            m_dynamic_queue.push_back(id); // later
            --budget;
            continue;
        }
        m_dynamic_queued.erase(id);
        const u32 cost = sync_entity(scene, id, made, gone);
        budget -= std::min(cost, budget);
    }
    if (!made.empty()) {
        std::vector<BodyDesc> descs;
        descs.reserve(made.size());
        for (Made& m : made) descs.push_back(std::move(m.desc));
        const std::vector<BodyId> ids = m_physics.add_many(descs);
        for (usize k = 0; k < made.size(); ++k) {
            Tracked& t = m_entities[made[k].id];
            t.body = ids[k];
            t.dynamic = made[k].dynamic && ids[k] != k_no_body;
            if (ids[k] != k_no_body) m_by_body[ids[k]] = made[k].id;
        }
    }
    // Old bodies go after the new ones are in (what rested on them wakes).
    m_physics.remove_many(gone);
}

void ScenePhysics::sync_tiles(world::Terrain& terrain, u32 budget) {
    const world::TerrainChanges changes = terrain.take_physics_changes();
    if (changes.all) {
        for (const auto& [cell, heights] : terrain.tiles()) enqueue_tile(cell);
        for (const auto& [cell, tile] : m_tiles) enqueue_tile(cell);
    } else {
        for (const world::CellCoord c : changes.tiles) {
            // The tile, and those whose far edge is its first row or column.
            enqueue_tile(c);
            enqueue_tile({c.x - 1, c.z});
            enqueue_tile({c.x, c.z - 1});
            enqueue_tile({c.x - 1, c.z - 1});
        }
    }
    const u32 n = terrain.samples();
    std::vector<BodyId> gone;
    while (budget > 0 && !m_tile_queue.empty()) {
        const world::CellCoord cell = m_tile_queue.front();
        m_tile_queue.pop_front();
        m_tile_queued.erase(cell);
        const world::Terrain::Heights* own = terrain.find(cell);
        if (own == nullptr) {
            const auto it = m_tiles.find(cell);
            if (it != m_tiles.end()) {
                if (it->second.body != k_no_body) gone.push_back(it->second.body);
                if (it->second.building != nullptr) it->second.building->superseded.store(true, std::memory_order_relaxed);
                m_tiles.erase(it); // a build on its way finishes into nothing
            }
            continue;
        }
        const u64 versions[4] = {terrain.version(cell), terrain.version({cell.x + 1, cell.z}),
                                 terrain.version({cell.x, cell.z + 1}), terrain.version({cell.x + 1, cell.z + 1})};
        Tile& t = m_tiles[cell];
        const u64* have = t.building != nullptr ? t.building->versions : t.versions;
        if ((t.body != k_no_body || t.building != nullptr) && std::equal(versions, versions + 4, have)) continue;
        --budget;
        // The tile's samples and the next tiles' first row and column, as the
        // finest mesh draws it (no seams between tiles); NaN where no ground.
        auto build = std::make_shared<Build>();
        std::copy(versions, versions + 4, build->versions);
        HeightShape h;
        h.samples = n + 1;
        h.spacing = static_cast<float>(terrain.spacing());
        h.heights.resize(static_cast<usize>(h.samples) * h.samples, std::numeric_limits<float>::quiet_NaN());
        for (u32 j = 0; j < n; ++j) {
            std::copy_n(own->begin() + static_cast<std::ptrdiff_t>(j) * n, n,
                        h.heights.begin() + static_cast<std::ptrdiff_t>(j) * h.samples);
        }
        const i64 i0 = static_cast<i64>(cell.x) * n;
        const i64 j0 = static_cast<i64>(cell.z) * n;
        for (u32 k = 0; k <= n; ++k) {
            if (const std::optional<float> v = terrain.sample(i0 + n, j0 + k, &cell)) {
                h.heights[static_cast<usize>(k) * h.samples + n] = *v; // the +x edge
            }
            if (const std::optional<float> v = terrain.sample(i0 + k, j0 + n, &cell)) {
                h.heights[static_cast<usize>(n) * h.samples + k] = *v; // the +z edge
            }
        }
        const double tile = terrain.tile_size();
        build->desc.position = {static_cast<double>(cell.x) * tile, 0.0, static_cast<double>(cell.z) * tile};
        build->desc.parts.push_back({std::move(h)});
        build->desc.friction = 0.8f;
        if (t.building == nullptr) {
            m_building.push_back(cell);
        } else {
            t.building->superseded.store(true, std::memory_order_relaxed); // else already listed
        }
        t.building = build;
        m_builder.submit(m_builds, [build, &physics = m_physics] {
            if (!build->superseded.load(std::memory_order_relaxed)) build->shape = physics.prepare(build->desc);
            build->desc.parts.clear(); // the samples are in the shape now
            build->done.store(true, std::memory_order_release);
        });
    }
    m_physics.remove_many(gone);
}

void ScenePhysics::finish_tiles(bool wait) {
    if (m_building.empty()) return;
    if (wait) m_builder.wait(m_builds);
    std::vector<world::CellCoord> ready;
    std::vector<BodyDesc> descs;
    std::vector<PreparedShape> shapes;
    std::vector<BodyId> gone;
    std::erase_if(m_building, [&](world::CellCoord cell) {
        const auto it = m_tiles.find(cell);
        if (it == m_tiles.end() || it->second.building == nullptr) return true; // gone meanwhile
        Build& b = *it->second.building;
        if (!b.done.load(std::memory_order_acquire)) return false;
        ready.push_back(cell);
        descs.push_back(b.desc);
        shapes.push_back(b.shape);
        return true;
    });
    if (ready.empty()) return;
    const std::vector<BodyId> ids = m_physics.add_many(descs, shapes);
    for (usize k = 0; k < ready.size(); ++k) {
        Tile& t = m_tiles[ready[k]];
        if (t.body != k_no_body) gone.push_back(t.body); // the old ground held until now
        t.body = ids[k];
        std::copy(t.building->versions, t.building->versions + 4, t.versions);
        t.building.reset();
    }
    m_physics.remove_many(gone);
}

void ScenePhysics::sync(world::Scene& scene, std::span<const world::EntityId> changed, bool all) {
    if (scene.identity() != m_scene) {
        clear();
        m_scene = scene.identity();
        all = true;
    }
    if (!(scene.kits == m_kits)) {
        // A kit's pieces changed: its placements are made again.
        const std::vector<std::string> kits = changed_kits(m_kits, scene.kits);
        m_kits = scene.kits;
        for (auto& [id, t] : m_entities) {
            if (t.look.shape == world::Shape::kit && std::find(kits.begin(), kits.end(), t.look.kit) != kits.end()) {
                t.remake = true;
                enqueue(scene, id);
            }
        }
    }
    if (all) {
        // Everything: what is in the scene (looked at in turn, a share a
        // sync), and what is no longer.
        m_scan.clear();
        m_scan.reserve(scene.entities().size());
        for (const world::Entity& e : scene.entities()) m_scan.push_back(e.id);
        m_scan_at = 0;
        for (const auto& [id, t] : m_entities) {
            if (scene.find(id) == nullptr) enqueue(scene, id);
        }
        // Drawn where they were put until their bodies catch up.
        std::vector<world::EntityId> moved;
        for (const auto& [id, pose] : m_poses) moved.push_back(id);
        for (const world::EntityId id : moved) enqueue(scene, id);
    } else {
        for (const world::EntityId id : changed) enqueue(scene, id);
    }
    finish_tiles(false);
    sync_tiles(scene.terrain, m_settings.tiles_per_sync);
    sync_entities(scene, m_settings.entities_per_sync);
}

void ScenePhysics::flush(world::Scene& scene, std::span<const world::EntityId> changed, bool all) {
    sync(scene, changed, all);
    sync_tiles(scene.terrain, ~u32{0});
    finish_tiles(true);
    sync_entities(scene, ~u32{0});
}

usize ScenePhysics::update_poses() {
    usize moved = 0;
    for (const BodyId body : m_physics.moved()) {
        const auto owner = m_by_body.find(body);
        if (owner == m_by_body.end()) continue;
        const auto it = m_entities.find(owner->second);
        if (it == m_entities.end() || !it->second.dynamic) continue;
        // An edit waiting to be seen: its old body is not where it is drawn.
        if (m_edited.count(owner->second) != 0) continue;
        const std::optional<BodyState> s = m_physics.state(body);
        if (!s) continue;
        world::Transform& pose = m_poses[owner->second];
        pose.position = s->position;
        pose.rotation = normalize(s->rotation);
        pose.scale = it->second.look.transform.scale;
        m_redraw.push_back(owner->second);
        ++moved;
    }
    return moved;
}

void ScenePhysics::reset_poses() {
    for (const auto& [id, pose] : m_poses) {
        const auto it = m_entities.find(id);
        if (it != m_entities.end() && it->second.body != k_no_body) {
            m_physics.set_transform(it->second.body, it->second.look.transform.position,
                                    it->second.look.transform.rotation);
            m_physics.set_velocity(it->second.body, {}, {});
        }
        m_redraw.push_back(id);
    }
    m_poses.clear();
}

std::vector<world::EntityId> ScenePhysics::take_redraw() {
    std::sort(m_redraw.begin(), m_redraw.end());
    m_redraw.erase(std::unique(m_redraw.begin(), m_redraw.end()), m_redraw.end());
    std::vector<world::EntityId> out = std::move(m_redraw);
    m_redraw = {};
    return out;
}

const world::Transform* ScenePhysics::placed(world::EntityId entity) const {
    const auto it = m_entities.find(entity);
    return it == m_entities.end() ? nullptr : &it->second.look.transform;
}

world::EntityId ScenePhysics::entity_of(BodyId body) const {
    const auto it = m_by_body.find(body);
    return it == m_by_body.end() ? 0 : it->second;
}

BodyId ScenePhysics::body_of(world::EntityId entity) const {
    const auto it = m_entities.find(entity);
    return it == m_entities.end() ? k_no_body : it->second.body;
}

ScenePhysicsStats ScenePhysics::stats() const {
    ScenePhysicsStats s;
    for (const auto& [id, t] : m_entities) {
        if (t.body != k_no_body) {
            ++s.entity_bodies;
            s.dynamic_bodies += t.dynamic ? 1u : 0u;
        } else if (t.look.body == world::BodyKind::none || t.look.shape == world::Shape::mesh ||
                   t.look.shape == world::Shape::decal) {
            ++s.not_colliding;
        } else {
            ++s.refused;
        }
    }
    for (const auto& [cell, tile] : m_tiles) s.tile_bodies += tile.body != k_no_body ? 1u : 0u;
    s.waiting = static_cast<u32>(m_queued.size() + m_dynamic_queue.size() + (m_scan.size() - m_scan_at));
    s.tiles_waiting = static_cast<u32>(m_tile_queued.size() + m_building.size());
    s.posed = static_cast<u32>(m_poses.size());
    return s;
}

} // namespace giant::sim
