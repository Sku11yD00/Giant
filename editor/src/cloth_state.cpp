#include "cloth_state.h"

#include "giant/core/log.h"
#include "giant/geometry/clusters.h"

#include <algorithm>
#include <string>

namespace giant::editor {

geometry::SkinnedTriangleMesh cloth_mesh(const sim::Cloths::Sheet& sheet) {
    geometry::SkinnedTriangleMesh out;
    const u32 w = sheet.columns(), h = sheet.rows(), n = w * h;
    const std::vector<Vec3>& rest = sheet.rest();
    out.mesh.vertices.reserve(2 * n);
    out.weights.reserve(2 * n);
    for (u32 side = 0; side < 2; ++side) {
        const Vec3 normal{0.0f, 0.0f, side == 0 ? 1.0f : -1.0f};
        for (u32 i = 0; i < n; ++i) {
            out.mesh.vertices.push_back({rest[i], normal});
            geometry::SkinWeights weight;
            weight.joints = {static_cast<u16>(i), 0, 0, 0};
            weight.weights = {255, 0, 0, 0};
            out.weights.push_back(weight);
        }
    }
    // Counter-clockwise seen from +z for the front; the back the other way.
    out.mesh.indices.reserve(static_cast<usize>(12) * (w - 1) * (h - 1));
    for (u32 r = 0; r + 1 < h; ++r) {
        for (u32 c = 0; c + 1 < w; ++c) {
            const u32 a = r * w + c, b = (r + 1) * w + c, d = (r + 1) * w + c + 1, e = r * w + c + 1;
            out.mesh.indices.insert(out.mesh.indices.end(), {a, b, d, a, d, e});
            out.mesh.indices.insert(out.mesh.indices.end(), {n + a, n + d, n + b, n + a, n + e, n + d});
        }
    }
    return out;
}

ClothState::ClothState(render::Renderer* renderer) : m_renderer(renderer) {}

ClothState::~ClothState() {
    if (m_renderer == nullptr) return;
    for (const auto& [id, d] : m_made) m_renderer->remove_mesh(d.mesh);
}

void ClothState::drop(world::EntityId id) {
    const auto it = m_made.find(id);
    if (it != m_made.end()) {
        if (m_renderer != nullptr) m_renderer->remove_mesh(it->second.mesh);
        m_made.erase(it);
    }
    m_drawn.erase(id);
}

void ClothState::follow_one(const world::Scene& scene, world::EntityId id) {
    const world::Entity* e = scene.find(id);
    if (e == nullptr || !e->visible || e->shape != world::Shape::cloth) {
        m_wanted.erase(id);
        m_cloths.remove(id);
        drop(id);
        return;
    }
    m_wanted.insert(id);
    m_cloths.set(id, e->cloth, e->transform);
    // A sheet of another size needs another mesh (its points rest elsewhere).
    const auto it = m_made.find(id);
    const sim::Cloths::Sheet* sheet = m_cloths.find(id);
    if (it != m_made.end() && sheet != nullptr &&
        (it->second.width != sheet->settings().width || it->second.height != sheet->settings().height)) {
        drop(id);
        m_redraw.push_back(id); // drawn with nothing until the new mesh is loaded
    }
}

void ClothState::follow(const world::Scene& scene, const world::SceneChanges& changed) {
    if (scene.identity() != m_scene || changed.all) {
        m_scene = scene.identity();
        std::vector<world::EntityId> ids(m_wanted.begin(), m_wanted.end());
        for (const world::Entity& e : scene.entities()) {
            if (e.shape == world::Shape::cloth) ids.push_back(e.id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const world::EntityId id : ids) follow_one(scene, id);
        return;
    }
    for (const world::EntityId id : changed.ids) follow_one(scene, id);
}

void ClothState::update(const WindState& wind, const DVec3& camera, double seconds) {
    m_cloths.step(seconds, wind, camera, &m_jobs);
    if (m_renderer == nullptr) return;
    // Meshes for the lowest ids waiting, a few a frame, within the limit.
    u32 made = 0;
    for (const world::EntityId id : m_wanted) {
        if (made >= meshes_per_frame || m_made.size() >= k_max_cloths) break;
        if (m_made.count(id) != 0) continue;
        const sim::Cloths::Sheet* sheet = m_cloths.find(id);
        if (sheet == nullptr) continue;
        geometry::SkinnedClusterMesh clusters = geometry::build_skinned_clusters(cloth_mesh(*sheet));
        ++made;
        Drawn d;
        d.mesh = m_renderer->add_skinned_mesh(std::move(clusters), static_cast<u32>(sheet->points().size()));
        if (d.mesh == render::k_no_mesh) {
            log_warning("editor", "cloth " + std::to_string(id) + ": the renderer refused its mesh");
            continue; // tried again next frame
        }
        d.width = sheet->settings().width;
        d.height = sheet->settings().height;
        m_made[id] = d;
    }
    // Those that moved posed; those loaded and posed drawn.
    for (auto& [id, d] : m_made) {
        const sim::Cloths::Sheet* sheet = m_cloths.find(id);
        if (sheet == nullptr) continue;
        if (sheet->version() != d.sent) {
            sheet->skin(m_skin);
            const geometry::Sphere bounds{sheet->bounds_center(), sheet->bounds_radius()};
            if (m_renderer->set_pose(d.mesh, m_skin, bounds)) {
                d.sent = sheet->version();
                ++m_poses;
            }
        }
        if (!d.ready && d.sent != 0 && m_renderer->mesh_ready(d.mesh)) {
            d.ready = true;
            m_drawn[id] = d.mesh;
            m_redraw.push_back(id);
        }
    }
}

std::vector<world::EntityId> ClothState::take_redraw() {
    std::vector<world::EntityId> out = std::move(m_redraw);
    m_redraw.clear();
    return out;
}

usize ClothState::waiting() const { return m_wanted.size() - std::min(m_wanted.size(), m_drawn.size()); }

} // namespace giant::editor
