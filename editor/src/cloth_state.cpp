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
    for (const auto& [id, m] : m_made) {
        m_renderer->remove_mesh(m.current.mesh);
        if (m.replacement.mesh != render::k_no_mesh) m_renderer->remove_mesh(m.replacement.mesh);
    }
}

void ClothState::drop(world::EntityId id) {
    const auto it = m_made.find(id);
    if (it != m_made.end()) {
        if (m_renderer != nullptr) {
            m_renderer->remove_mesh(it->second.current.mesh);
            if (it->second.replacement.mesh != render::k_no_mesh) m_renderer->remove_mesh(it->second.replacement.mesh);
        }
        m_made.erase(it);
    }
    m_drawn.erase(id);
}

void ClothState::follow_one(const world::Scene& scene, world::EntityId id) {
    m_refused.erase(id); // changed: tried again
    const world::Entity* e = scene.find(id);
    if (e == nullptr || !e->visible || e->shape != world::Shape::cloth) {
        m_wanted.erase(id);
        m_cloths.remove(id);
        drop(id);
        return;
    }
    m_wanted.insert(id);
    m_cloths.set(id, e->cloth, e->transform);
}

void ClothState::follow(const world::Scene& scene, const world::SceneChanges& changed) {
    // Replacements loaded since: drawn instead of the old meshes from this
    // frame's cells on (their cells are built after this).
    for (const world::EntityId id : m_swaps) {
        const auto it = m_made.find(id);
        if (it == m_made.end() || !it->second.replacement.ready) continue;
        Made& m = it->second;
        if (m_renderer != nullptr) m_renderer->remove_mesh(m.current.mesh);
        m.current = m.replacement;
        m.replacement = {};
        m_drawn[id] = m.current.mesh;
        m_redraw.push_back(id);
    }
    m_swaps.clear();
    catch_up(scene, changed);
}

void ClothState::catch_up(const world::Scene& scene, const world::SceneChanges& changed) {
    if (scene.identity() != m_scene || changed.all) {
        m_scene = scene.identity();
        m_refused.clear();
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

ClothState::Drawn ClothState::make(world::EntityId id, const sim::Cloths::Sheet& sheet) {
    Drawn d;
    d.mesh = m_renderer->add_skinned_mesh(geometry::build_skinned_clusters(cloth_mesh(sheet)),
                                          static_cast<u32>(sheet.points().size()));
    if (d.mesh == render::k_no_mesh) {
        log_warning("editor", "cloth " + std::to_string(id) + ": the renderer refused its mesh (tried again when it changes)");
        m_refused.insert(id);
        return d;
    }
    d.width = sheet.settings().width;
    d.height = sheet.settings().height;
    return d;
}

void ClothState::pose(Drawn& d, const sim::Cloths::Sheet& sheet) {
    if (d.mesh == render::k_no_mesh || d.width != sheet.settings().width || d.height != sheet.settings().height ||
        sheet.version() == d.sent) {
        return; // made for another size (its old pose stays), or as last posed
    }
    sheet.skin(m_skin);
    const geometry::Sphere bounds{sheet.bounds_center(), sheet.bounds_radius()};
    if (m_renderer->set_pose(d.mesh, m_skin, bounds)) {
        d.sent = sheet.version();
        ++m_poses;
    }
}

void ClothState::update(const WindState& wind, const DVec3& camera, double seconds) {
    m_cloths.step(seconds, wind, camera, &m_jobs);
    if (m_renderer == nullptr) return;
    // Meshes for the lowest ids waiting (or resized), a few a frame, within the limit.
    u32 made = 0;
    for (const world::EntityId id : m_wanted) {
        if (made >= meshes_per_frame) break;
        if (m_refused.count(id) != 0) continue;
        const sim::Cloths::Sheet* sheet = m_cloths.find(id);
        if (sheet == nullptr) continue;
        const float w = sheet->settings().width, h = sheet->settings().height;
        const auto it = m_made.find(id);
        if (it == m_made.end()) {
            if (m_made.size() >= k_max_cloths) continue;
            ++made;
            const Drawn d = make(id, *sheet);
            if (d.mesh != render::k_no_mesh) m_made[id].current = d;
            continue;
        }
        Made& m = it->second;
        const bool current_fits = m.current.width == w && m.current.height == h;
        Drawn& r = m.replacement;
        const bool replacement_fits = r.mesh != render::k_no_mesh && r.width == w && r.height == h;
        if (current_fits) {
            if (r.mesh != render::k_no_mesh) m_renderer->remove_mesh(r.mesh); // resized back: never drawn
            r = {};
            continue;
        }
        if (replacement_fits) continue;
        if (r.mesh != render::k_no_mesh) m_renderer->remove_mesh(r.mesh); // for a size it no longer has: never drawn
        ++made;
        r = make(id, *sheet);
    }
    // Those that moved posed; those loaded and posed drawn.
    for (auto& [id, m] : m_made) {
        const sim::Cloths::Sheet* sheet = m_cloths.find(id);
        if (sheet == nullptr) continue;
        pose(m.current, *sheet);
        pose(m.replacement, *sheet);
        if (!m.current.ready && m.current.sent != 0 && m_renderer->mesh_ready(m.current.mesh)) {
            m.current.ready = true;
            m_drawn[id] = m.current.mesh;
            m_redraw.push_back(id);
        }
        Drawn& r = m.replacement;
        if (r.mesh != render::k_no_mesh && !r.ready && r.sent != 0 && m_renderer->mesh_ready(r.mesh)) {
            r.ready = true;
            m_swaps.push_back(id);
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
