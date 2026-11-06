#include "scene_render.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace giant::editor {

namespace {

render::BuiltinMesh builtin(world::Shape shape) {
    switch (shape) {
    case world::Shape::sphere: return render::BuiltinMesh::sphere;
    case world::Shape::plane: return render::BuiltinMesh::plane;
    default: break;
    }
    return render::BuiltinMesh::cube;
}

// The material's index in the table, adding it if new, and one more user.
u32 material_index(const world::Material& m, SceneFrame& out) {
    SceneFrame::MaterialKey key{};
    const float values[8] = {m.base_color.x, m.base_color.y, m.base_color.z, m.roughness,
                             m.metallic,     m.emissive.x,   m.emissive.y,   m.emissive.z};
    std::memcpy(key.bits, values, sizeof(values));
    const auto [it, added] = out.lookup.try_emplace(key, static_cast<u32>(out.materials.size()));
    if (added) {
        render::Material r;
        r.base_color = m.base_color;
        r.roughness = m.roughness;
        r.metallic = m.metallic;
        r.emissive = m.emissive;
        out.materials.push_back(r);
        out.users.push_back(0);
    }
    ++out.users[it->second];
    return it->second;
}

render::DecalShape decal_shape(world::DecalShape s) {
    switch (s) {
    case world::DecalShape::disc: return render::DecalShape::disc;
    case world::DecalShape::rectangle: return render::DecalShape::rectangle;
    case world::DecalShape::ring: return render::DecalShape::ring;
    case world::DecalShape::splat: return render::DecalShape::splat;
    case world::DecalShape::cracks: return render::DecalShape::cracks;
    }
    return render::DecalShape::disc;
}

// One drawable thing (an entity, or a piece of a placed kit).
void emit(world::Shape shape, const std::string& mesh_path, const world::Transform& t, const world::Material& m,
          const world::DecalSettings& d, const render::Renderer& renderer, const MeshLibrary& meshes,
          SceneFrame& out, SceneFrame::Cell& cell) {
    if (shape == world::Shape::decal) {
        render::Decal decal;
        decal.position = t.position;
        decal.rotation = t.rotation;
        decal.size = t.scale;
        decal.base_color = m.base_color;
        decal.roughness = m.roughness;
        decal.metallic = m.metallic;
        decal.emissive = m.emissive;
        decal.opacity = d.opacity;
        decal.softness = d.softness;
        decal.shape = decal_shape(d.shape);
        decal.seed = d.seed;
        cell.decals.push_back(decal);
        return;
    }
    render::MeshId mesh = render::k_no_mesh;
    if (shape == world::Shape::mesh) {
        const MeshLibrary::Mesh* found = meshes.find_normalized(mesh_path);
        if (found == nullptr || found->id == render::k_no_mesh) {
            return;
        }
        mesh = found->id;
    } else if (shape == world::Shape::kit) {
        return; // kits do not nest
    } else {
        mesh = renderer.builtin(builtin(shape));
    }
    render::Instance i;
    i.position = t.position;
    i.rotation = t.rotation;
    i.scale = t.scale;
    i.mesh = mesh;
    i.material = material_index(m, out);
    cell.instances.push_back(i);
}

// Rebuilds a cell's instances and decals from its entities. False if a
// material lost its last user (the table should be compacted).
bool rebuild(const world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
             SceneFrame& out, SceneFrame::Cell& cell) {
    std::vector<u32>& before = out.scratch;
    before.clear();
    for (const render::Instance& i : cell.instances) {
        if (i.material < out.users.size()) {
            --out.users[i.material];
            before.push_back(i.material);
        }
    }
    cell.instances.clear();
    cell.decals.clear();
    std::sort(cell.ids.begin(), cell.ids.end());
    for (const world::EntityId id : cell.ids) {
        const world::Entity* e = scene.find(id);
        if (e == nullptr || !e->visible) {
            continue;
        }
        if (e->shape != world::Shape::kit) {
            emit(e->shape, e->mesh, e->transform, e->material, e->decal, renderer, meshes, out, cell);
            continue;
        }
        const world::Kit* kit = scene.find_kit(e->kit);
        if (kit == nullptr) {
            continue;
        }
        for (const world::KitPiece& p : kit->pieces) {
            emit(p.shape, p.mesh, place(e->transform, p.transform), p.material, p.decal, renderer, meshes, out,
                 cell);
        }
    }
    // Unique in the process: a renderer handed cells by another SceneFrame
    // never takes one version for another.
    static std::atomic<u64> next_version{1};
    cell.version = next_version.fetch_add(1, std::memory_order_relaxed);
    cell.dirty = false;
    ++out.cells_rebuilt;
    for (const u32 m : before) {
        if (out.users[m] == 0) {
            return false;
        }
    }
    return true;
}

// Every entity into its cell, all of them to be built; an empty material table.
void start_over(const world::Scene& scene, SceneFrame& out) {
    out.grid.clear();
    out.where.clear();
    out.lookup.clear();
    out.materials.clear();
    out.users.clear();
    out.order.clear();
    for (const world::Entity& e : scene.entities()) {
        const world::CellCoord c = world::cell_of(e.transform.position);
        out.grid[c].ids.push_back(e.id);
        out.where[e.id] = c;
    }
}

bool same_kit(const world::Kit& a, const world::Kit& b) {
    if (a.name != b.name || a.pieces.size() != b.pieces.size()) {
        return false;
    }
    for (usize i = 0; i < a.pieces.size(); ++i) {
        const world::KitPiece& p = a.pieces[i];
        const world::KitPiece& q = b.pieces[i];
        const auto same3 = [](const auto& u, const auto& v) { return u.x == v.x && u.y == v.y && u.z == v.z; };
        if (p.shape != q.shape || p.mesh != q.mesh || !same3(p.transform.position, q.transform.position) ||
            !(p.transform.rotation == q.transform.rotation) || !same3(p.transform.scale, q.transform.scale) ||
            !same3(p.material.base_color, q.material.base_color) || p.material.roughness != q.material.roughness ||
            p.material.metallic != q.material.metallic || !same3(p.material.emissive, q.material.emissive) ||
            p.decal.shape != q.decal.shape || p.decal.opacity != q.decal.opacity ||
            p.decal.softness != q.decal.softness || p.decal.seed != q.decal.seed) {
            return false;
        }
    }
    return true;
}

// The names of kits added, removed or changed since `before`.
std::vector<std::string> changed_kits(const std::vector<world::Kit>& before, const std::vector<world::Kit>& now) {
    std::vector<std::string> names;
    std::unordered_map<std::string, const world::Kit*> old;
    for (const world::Kit& k : before) {
        old[k.name] = &k;
    }
    for (const world::Kit& k : now) {
        const auto it = old.find(k.name);
        if (it == old.end() || !same_kit(*it->second, k)) {
            names.push_back(k.name);
        }
        if (it != old.end()) {
            old.erase(it);
        }
    }
    for (const auto& [name, kit] : old) {
        names.push_back(name);
    }
    return names;
}

} // namespace

usize SceneFrame::MaterialHash::operator()(const MaterialKey& k) const {
    u64 h = 1469598103934665603ull;
    for (const u32 b : k.bits) {
        h = (h ^ b) * 1099511628211ull;
    }
    return static_cast<usize>(h);
}

world::Transform place(const world::Transform& parent, const world::Transform& child) {
    world::Transform out;
    // Pieces sit within a few hundred metres of their kit: float is plenty.
    const Vec3 scaled{static_cast<float>(child.position.x) * parent.scale.x,
                      static_cast<float>(child.position.y) * parent.scale.y,
                      static_cast<float>(child.position.z) * parent.scale.z};
    const Vec3 offset = rotate(parent.rotation, scaled);
    out.position = parent.position + DVec3{offset.x, offset.y, offset.z};
    out.rotation = normalize(parent.rotation * child.rotation);
    out.scale = {parent.scale.x * child.scale.x, parent.scale.y * child.scale.y, parent.scale.z * child.scale.z};
    return out;
}

void scene_frame(world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
                 SceneFrame& out) {
    out.cells_rebuilt = 0;
    world::SceneChanges changes = scene.take_changes();
    if (changes.all || scene.identity() != out.scene_identity || meshes.version() != out.meshes_version) {
        out.scene_identity = scene.identity();
        out.meshes_version = meshes.version();
        out.kits = scene.kits;
        start_over(scene, out);
    } else {
        // Entities added, removed, changed or moved to another cell.
        for (const world::EntityId id : changes.ids) {
            const world::Entity* e = scene.find(id);
            const auto was = out.where.find(id);
            if (was != out.where.end()) {
                SceneFrame::Cell& old = out.grid[was->second];
                old.dirty = true;
                if (e == nullptr || !(world::cell_of(e->transform.position) == was->second)) {
                    std::erase(old.ids, id);
                    out.where.erase(was);
                } else {
                    continue; // changed in place
                }
            }
            if (e != nullptr) {
                const world::CellCoord c = world::cell_of(e->transform.position);
                SceneFrame::Cell& now = out.grid[c];
                now.ids.push_back(id);
                now.dirty = true;
                out.where[id] = c;
            }
        }
        // Kits changed: the cells placing them.
        const std::vector<std::string> kits = changed_kits(out.kits, scene.kits);
        if (!kits.empty()) {
            out.kits = scene.kits;
            for (auto& [coord, cell] : out.grid) {
                for (usize i = 0; i < cell.ids.size() && !cell.dirty; ++i) {
                    const world::Entity* e = scene.find(cell.ids[i]);
                    cell.dirty = e != nullptr && e->shape == world::Shape::kit &&
                                 std::find(kits.begin(), kits.end(), e->kit) != kits.end();
                }
            }
        }
    }
    // Rebuild what changed. A material nothing uses any more: start over,
    // so the table holds only what is drawn.
    bool layout = out.order.empty();
    bool compact = false;
    for (auto it = out.grid.begin(); it != out.grid.end();) {
        SceneFrame::Cell& cell = it->second;
        if (cell.dirty) {
            const usize count = cell.instances.size();
            const bool fresh = cell.version == 0; // not laid out yet, even with no instances
            compact = !rebuild(scene, renderer, meshes, out, cell) || compact;
            layout = layout || fresh || cell.instances.size() != count;
        }
        if (cell.ids.empty()) {
            layout = true;
            it = out.grid.erase(it);
        } else {
            ++it;
        }
    }
    if (compact) {
        start_over(scene, out);
        out.cells_rebuilt = 0; // counted once, by the full rebuild
        for (auto& [coord, cell] : out.grid) {
            rebuild(scene, renderer, meshes, out, cell);
        }
        layout = true;
    }
    if (layout) {
        out.order.clear();
        out.order.reserve(out.grid.size());
        for (const auto& [coord, cell] : out.grid) {
            out.order.push_back(coord);
        }
        std::sort(out.order.begin(), out.order.end(), [](const world::CellCoord& a, const world::CellCoord& b) {
            return a.z != b.z ? a.z < b.z : a.x < b.x;
        });
    }
    if (!layout && out.cells_rebuilt == 0) {
        return; // as last frame
    }
    // Lay the cells out end to end (only the rebuilt ones when none moved).
    if (layout) {
        out.instances.clear();
        out.cells.clear();
    }
    out.decals.clear();
    for (usize n = 0; n < out.order.size(); ++n) {
        const SceneFrame::Cell& cell = out.grid.at(out.order[n]);
        out.decals.insert(out.decals.end(), cell.decals.begin(), cell.decals.end());
        if (layout) {
            render::CellRange r;
            r.first = static_cast<u32>(out.instances.size());
            r.count = static_cast<u32>(cell.instances.size());
            r.version = cell.version;
            out.cells.push_back(r);
            out.instances.insert(out.instances.end(), cell.instances.begin(), cell.instances.end());
        } else if (out.cells[n].version != cell.version) {
            out.cells[n].version = cell.version;
            std::copy(cell.instances.begin(), cell.instances.end(),
                      out.instances.begin() + static_cast<std::ptrdiff_t>(out.cells[n].first));
        }
    }
}

render::Lighting scene_lighting(const world::Lighting& l) {
    render::Lighting out;
    out.sun_direction = world::sun_direction(l);
    // Below the horizon the sun stops lighting (night); ambient remains.
    const float sun = l.sun_elevation_degrees > -2.0f ? l.sun_intensity : 0.0f;
    out.sun_light = l.sun_color * sun;
    out.sky_light = l.sky_color * l.ambient_intensity;
    out.ground_light = l.ground_color * l.ambient_intensity;
    out.background = l.sky_color;
    out.exposure = l.exposure;
    return out;
}

} // namespace giant::editor
