#include "swing_state.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {

// One thing's own box (metres, before its transform's scale), or none.
bool own_size(world::Shape shape, const std::string& mesh, const render::Renderer* renderer,
              const MeshLibrary* meshes, Vec3& out) {
    switch (shape) {
    case world::Shape::cube:
    case world::Shape::sphere: out = {1.0f, 1.0f, 1.0f}; return true;
    case world::Shape::plane: out = {1.0f, 0.0f, 1.0f}; return true;
    case world::Shape::mesh: {
        out = {1.0f, 1.0f, 1.0f}; // not loaded (or no renderer): a box of its scale
        if (renderer == nullptr || meshes == nullptr) return true;
        const MeshLibrary::Mesh* found = meshes->find_normalized(mesh);
        if (found == nullptr || found->id == render::k_no_mesh) return true;
        const geometry::ClusterMesh* m = renderer->mesh(found->id);
        if (m == nullptr || !(m->bounds.radius > 0.0f)) return true;
        const float side = 1.2f * m->bounds.radius; // about the box its sphere holds
        out = {side, side, side};
        return true;
    }
    case world::Shape::kit:
    case world::Shape::decal:
    case world::Shape::cloth: return false;
    }
    return false;
}

Vec3 scaled(const Vec3& a, const Vec3& b) { return {std::fabs(a.x * b.x), std::fabs(a.y * b.y), std::fabs(a.z * b.z)}; }

} // namespace

Vec3 swing_size(const world::Scene& scene, const world::Entity& e, const render::Renderer* renderer,
                const MeshLibrary* meshes) {
    const Vec3 scale = e.transform.scale;
    if (e.shape != world::Shape::kit) {
        Vec3 own{};
        return own_size(e.shape, e.mesh, renderer, meshes, own) ? scaled(own, scale) : scaled({1, 1, 1}, scale);
    }
    const world::Kit* kit = scene.find_kit(e.kit);
    if (kit == nullptr) return scaled({1, 1, 1}, scale);
    // Around its pieces' boxes, in the kit's space.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const world::KitPiece& p : kit->pieces) {
        Vec3 own{};
        if (!own_size(p.shape, p.mesh, renderer, meshes, own)) continue;
        const Vec3 half = scaled(own, p.transform.scale) * 0.5f;
        const Vec3 ax = rotate(p.transform.rotation, Vec3{half.x, 0.0f, 0.0f});
        const Vec3 ay = rotate(p.transform.rotation, Vec3{0.0f, half.y, 0.0f});
        const Vec3 az = rotate(p.transform.rotation, Vec3{0.0f, 0.0f, half.z});
        const Vec3 reach{std::fabs(ax.x) + std::fabs(ay.x) + std::fabs(az.x),
                         std::fabs(ax.y) + std::fabs(ay.y) + std::fabs(az.y),
                         std::fabs(ax.z) + std::fabs(ay.z) + std::fabs(az.z)};
        const Vec3 c{static_cast<float>(p.transform.position.x), static_cast<float>(p.transform.position.y),
                     static_cast<float>(p.transform.position.z)};
        lo = {std::min(lo.x, c.x - reach.x), std::min(lo.y, c.y - reach.y), std::min(lo.z, c.z - reach.z)};
        hi = {std::max(hi.x, c.x + reach.x), std::max(hi.y, c.y + reach.y), std::max(hi.z, c.z + reach.z)};
    }
    if (!(lo.x <= hi.x)) return scaled({1, 1, 1}, scale);
    return scaled(hi - lo, scale);
}

SwingState::SwingState(const render::Renderer* renderer, const MeshLibrary* meshes)
    : m_renderer(renderer), m_meshes(meshes) {}

void SwingState::follow_one(const world::Scene& scene, world::EntityId id) {
    const world::Entity* e = scene.find(id);
    if (e == nullptr || !e->visible || !e->swing.on || !sim::can_swing(e->shape)) {
        m_swings.remove(id);
        m_meshed.erase(id);
        return;
    }
    if (e->shape == world::Shape::mesh || e->shape == world::Shape::kit) {
        m_meshed.insert(id);
    } else {
        m_meshed.erase(id);
    }
    m_swings.set(id, e->swing, e->transform, swing_size(scene, *e, m_renderer, m_meshes));
}

void SwingState::follow(const world::Scene& scene, const world::SceneChanges& changed) {
    if (scene.identity() != m_scene || changed.all) {
        const bool fresh = scene.identity() != m_scene;
        m_scene = scene.identity();
        std::vector<world::EntityId> ids;
        if (fresh) {
            m_swings.clear(); // another scene's: nothing carries over (the same ids are other things)
            m_meshed.clear();
        }
        for (const auto& [id, s] : m_swings.poses()) ids.push_back(id);
        for (const world::Entity& e : scene.entities()) {
            if (e.swing.on) ids.push_back(e.id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const world::EntityId id : ids) follow_one(scene, id);
    } else {
        for (const world::EntityId id : changed.ids) follow_one(scene, id);
    }
    // Meshes loaded or gone: the sizes of those that use them.
    const u64 meshes = m_meshes != nullptr ? m_meshes->version() : 0;
    if (meshes != m_meshes_version) {
        m_meshes_version = meshes;
        const std::vector<world::EntityId> ids(m_meshed.begin(), m_meshed.end());
        for (const world::EntityId id : ids) follow_one(scene, id);
    }
}

void SwingState::update(const WindState& wind, const DVec3& camera, double seconds) {
    m_swings.step(seconds, wind, camera);
}

} // namespace giant::editor
