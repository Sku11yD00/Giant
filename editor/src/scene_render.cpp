#include "scene_render.h"

#include "far_ground.h"
#include "scatter_placer.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace giant::editor {

namespace {

// Unique in the process: a renderer handed cells by another SceneFrame
// never takes one version for another.
u64 new_version() {
    static std::atomic<u64> next_version{1};
    return next_version.fetch_add(1, std::memory_order_relaxed);
}

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
    const float values[10] = {m.base_color.x, m.base_color.y, m.base_color.z, m.roughness, m.metallic,
                              m.emissive.x,   m.emissive.y,   m.emissive.z,   m.sway,      m.flutter};
    std::memcpy(key.bits, values, sizeof(values));
    const auto [it, added] = out.lookup.try_emplace(key, static_cast<u32>(out.materials.size()));
    if (added) {
        render::Material r;
        r.base_color = m.base_color;
        r.roughness = m.roughness;
        r.metallic = m.metallic;
        r.emissive = m.emissive;
        r.sway = m.sway;
        r.flutter = m.flutter;
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
          SceneFrame& out, SceneFrame::Cell& cell, render::MeshId own = render::k_no_mesh) {
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
    if (shape == world::Shape::mesh && own != render::k_no_mesh) {
        mesh = own; // an animated copy
    } else if (shape == world::Shape::mesh) {
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

// A cell's scatter placements: each item's mesh and material looked up
// once a cell (a forest cell is thousands of the same few things).
// Fallen trees (`fallen`, in point order) are drawn where they lie, after
// all the rest (so moving ones are made again in place: rebuild_fallen);
// `fallen_only` draws them alone.
void emit_scatter(const world::Scene& scene, const ScatterPlacer::Cell& placed, const render::Renderer& renderer,
                  const MeshLibrary& meshes, SceneFrame& out, SceneFrame::Cell& cell,
                  const std::vector<sim::FallenTree>* fallen, bool fallen_only = false) {
    struct Piece {
        const world::KitPiece* piece = nullptr;
        render::MeshId mesh = render::k_no_mesh; // k_no_mesh: a decal (emitted as one), or nothing
        u32 material = 0;
    };
    struct Look {
        bool known = false;
        render::MeshId mesh = render::k_no_mesh; // k_no_mesh: draws nothing (or a kit)
        u32 material = 0;
        const world::Kit* kit = nullptr;
        std::vector<Piece> pieces; // a kit's, looked up once
    };
    const auto put = [&](const world::Transform& t, render::MeshId mesh, u32 material) {
        render::Instance i;
        i.position = t.position;
        i.rotation = t.rotation;
        i.scale = t.scale;
        i.mesh = mesh;
        i.material = material;
        ++out.users[material];
        cell.instances.push_back(i);
    };
    const ScatterPlacer::Layers& layers = *placed.layers;
    std::vector<usize> first(layers.size() + 1, 0);
    for (usize k = 0; k < layers.size(); ++k) first[k + 1] = first[k] + layers[k].items.size();
    std::vector<Look> looks(first.back());
    // At most what the placer counted (a kit grown since draws its old
    // points' share until the cell is placed again).
    const usize most = cell.instances.size() + cell.decals.size() + placed.instances;
    // Every point but the fallen, then the fallen.
    const auto draw = [&](const world::ScatterPoint& p, const world::Transform& at) {
        const world::ScatterItem& item = layers[p.layer].items[p.item];
        Look& look = looks[first[p.layer] + p.item];
        if (!look.known) {
            look.known = true;
            if (item.shape == world::Shape::kit) {
                look.kit = scene.find_kit(item.kit);
                if (look.kit != nullptr) {
                    for (const world::KitPiece& piece : look.kit->pieces) {
                        Piece& lp = look.pieces.emplace_back();
                        lp.piece = &piece;
                        if (piece.shape == world::Shape::mesh) {
                            const MeshLibrary::Mesh* found = meshes.find_normalized(piece.mesh);
                            if (found != nullptr) lp.mesh = found->id;
                        } else if (piece.shape != world::Shape::decal) {
                            lp.mesh = renderer.builtin(builtin(piece.shape));
                        }
                        if (lp.mesh != render::k_no_mesh) {
                            lp.material = material_index(piece.material, out);
                            --out.users[lp.material]; // counted per instance below
                        }
                    }
                }
            } else if (item.shape == world::Shape::mesh) {
                const MeshLibrary::Mesh* found = meshes.find_normalized(item.mesh);
                if (found != nullptr) look.mesh = found->id;
            } else if (item.shape != world::Shape::decal) {
                look.mesh = renderer.builtin(builtin(item.shape));
            }
            if (look.mesh != render::k_no_mesh) {
                look.material = material_index(item.material, out);
                --out.users[look.material]; // counted per instance below
            }
        }
        if (look.kit != nullptr) {
            for (const Piece& lp : look.pieces) {
                const world::Transform t = place(at, lp.piece->transform);
                if (lp.mesh != render::k_no_mesh) {
                    put(t, lp.mesh, lp.material);
                } else if (lp.piece->shape == world::Shape::decal) {
                    emit(lp.piece->shape, lp.piece->mesh, t, lp.piece->material, lp.piece->decal, renderer, meshes,
                         out, cell);
                }
            }
            return;
        }
        if (look.mesh != render::k_no_mesh) put(at, look.mesh, look.material);
    };
    usize next_fallen = 0;
    for (u32 index = 0; index < placed.points.size() && !fallen_only; ++index) {
        if (cell.instances.size() + cell.decals.size() >= most) break;
        while (fallen != nullptr && next_fallen < fallen->size() && (*fallen)[next_fallen].point < index) ++next_fallen;
        if (fallen != nullptr && next_fallen < fallen->size() && (*fallen)[next_fallen].point == index) continue;
        draw(placed.points[index], placed.points[index].transform);
    }
    cell.fallen_first = static_cast<u32>(cell.instances.size());
    cell.fallen_decals = static_cast<u32>(cell.decals.size());
    if (fallen == nullptr) return;
    for (const sim::FallenTree& f : *fallen) {
        if (cell.instances.size() + cell.decals.size() >= most) break;
        if (f.point < placed.points.size()) draw(placed.points[f.point], f.at);
    }
}

// The cell's terrain tile, if its mesh is built (and the far ground draws
// it), and the far ground's chunks drawn from the cell.
void emit_ground(const world::Scene& scene, const TerrainMeshes* terrain, const FarGround* far,
                 world::CellCoord coord, SceneFrame& out, SceneFrame::Cell& cell) {
    cell.ground = false;
    if (terrain == nullptr) return;
    world::Material m;
    m.base_color = scene.terrain.material.base_color;
    m.roughness = scene.terrain.material.roughness;
    const auto put = [&](render::MeshId mesh, double x, double z) {
        render::Instance i;
        i.position = {x, 0.0, z};
        i.mesh = mesh;
        i.material = material_index(m, out);
        i.always_mesh = true;
        cell.instances.push_back(i);
        cell.ground = true;
    };
    const render::MeshId mesh = terrain->mesh(coord);
    if (mesh != render::k_no_mesh && scene.terrain.find(coord) != nullptr && (far == nullptr || far->draws_tile(coord))) {
        put(mesh, coord.x * world::k_cell_size, coord.z * world::k_cell_size);
    }
    if (far == nullptr) return;
    std::vector<FarGround::Piece> pieces;
    far->pieces(coord, pieces);
    for (const FarGround::Piece& p : pieces) {
        if (p.mesh == render::k_no_mesh) continue;
        const double size = world::far_tile_size(p.level);
        put(p.mesh, p.chunk.x * size, p.chunk.z * size);
    }
}

// Rebuilds a cell's instances and decals from its entities (and its
// terrain tile). False if a material lost its last user (the table should
// be compacted).
bool rebuild(const world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
             const TerrainMeshes* terrain, const FarGround* far, const ScatterPlacer* scatter, world::CellCoord coord,
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
    emit_ground(scene, terrain, far, coord, out, cell);
    cell.ground_count = static_cast<u32>(cell.instances.size());
    cell.ground_dirty = false;
    cell.prev_version = 0;
    std::sort(cell.ids.begin(), cell.ids.end());
    for (const world::EntityId id : cell.ids) {
        const world::Entity* e = scene.find(id);
        if (e == nullptr || !e->visible) {
            continue;
        }
        const world::Transform* posed = nullptr;
        if (out.poses != nullptr) {
            const auto p = out.poses->find(id);
            if (p != out.poses->end()) posed = &p->second;
        }
        const world::Transform& at = posed != nullptr ? *posed : e->transform;
        if (e->shape != world::Shape::kit) {
            render::MeshId own = render::k_no_mesh;
            if (out.skinned != nullptr && e->shape == world::Shape::mesh) {
                const auto s = out.skinned->find(id);
                if (s != out.skinned->end()) own = s->second;
            }
            emit(e->shape, e->mesh, at, e->material, e->decal, renderer, meshes, out, cell, own);
            continue;
        }
        const world::Kit* kit = scene.find_kit(e->kit);
        if (kit == nullptr) {
            continue;
        }
        // Loose pieces (breaking) where they fell; the rest where they stand.
        const std::vector<sim::PiecePose>* loose = nullptr;
        if (out.piece_poses != nullptr) {
            const auto l = out.piece_poses->find(id);
            if (l != out.piece_poses->end()) loose = &l->second;
        }
        usize next = 0;
        for (u32 i = 0; i < kit->pieces.size(); ++i) {
            const world::KitPiece& p = kit->pieces[i];
            while (loose != nullptr && next < loose->size() && (*loose)[next].piece < i) ++next;
            const bool fell = loose != nullptr && next < loose->size() && (*loose)[next].piece == i;
            emit(p.shape, p.mesh, fell ? (*loose)[next].at : place(at, p.transform), p.material, p.decal, renderer,
                 meshes, out, cell);
        }
    }
    cell.scattered = false;
    cell.fallen_points.clear();
    if (scatter != nullptr) {
        if (const ScatterPlacer::Cell* placed = scatter->find(coord)) {
            cell.scattered = true;
            const std::vector<sim::FallenTree>* fallen = nullptr;
            if (out.fallen != nullptr) {
                const auto f = out.fallen->find(coord);
                if (f != out.fallen->end() && f->second.serial == placed->serial) fallen = &f->second.trees;
            }
            emit_scatter(scene, *placed, renderer, meshes, out, cell, fallen);
            cell.fallen_points.clear();
            if (fallen != nullptr) {
                for (const sim::FallenTree& f : *fallen) cell.fallen_points.push_back(f.point);
            }
        }
    }
    cell.version = new_version();
    cell.dirty = false;
    cell.fallen_dirty = false;
    ++out.cells_rebuilt;
    for (const u32 m : before) {
        if (out.users[m] == 0) {
            return false;
        }
    }
    return true;
}

// Makes a cell's ground instances again in place when their count is as
// before (the rest of the cell kept, its place too); false when it needs a
// full rebuild. `compact`: a material lost its last user.
bool rebuild_ground(const world::Scene& scene, const TerrainMeshes* terrain, const FarGround* far,
                    world::CellCoord coord, SceneFrame& out, SceneFrame::Cell& cell, bool& compact) {
    if (cell.version == 0 || !cell.placed || cell.ground_count > cell.instances.size()) {
        return false;
    }
    SceneFrame::Cell fresh;
    emit_ground(scene, terrain, far, coord, out, fresh); // counts its materials' users
    if (fresh.instances.size() != cell.ground_count || !fresh.decals.empty()) {
        for (const render::Instance& i : fresh.instances) {
            if (i.material < out.users.size()) --out.users[i.material]; // not used after all
        }
        return false;
    }
    for (u32 k = 0; k < cell.ground_count; ++k) {
        const u32 m = cell.instances[k].material;
        if (m < out.users.size() && --out.users[m] == 0) compact = true;
        cell.instances[k] = fresh.instances[k];
        out.instances[cell.first + k] = fresh.instances[k];
    }
    cell.ground = fresh.ground;
    cell.prev_version = cell.version;
    cell.changed_first = 0;
    cell.changed_count = cell.ground_count;
    cell.version = new_version();
    cell.ground_dirty = false;
    ++out.cells_rebuilt;
    return true;
}

// Makes a cell's fallen trees' instances again in place when the same
// trees are fallen and they draw as many instances as before (the rest of
// the cell kept, its place too); false when it needs a full rebuild.
// `compact`: a material lost its last user.
bool rebuild_fallen(const world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
                    const ScatterPlacer* scatter, world::CellCoord coord, SceneFrame& out, SceneFrame::Cell& cell,
                    bool& compact) {
    if (cell.version == 0 || !cell.placed || !cell.scattered || scatter == nullptr || out.fallen == nullptr ||
        cell.fallen_first > cell.instances.size() || cell.fallen_decals != cell.decals.size()) {
        return false;
    }
    const ScatterPlacer::Cell* placed = scatter->find(coord);
    const auto f = out.fallen->find(coord);
    if (placed == nullptr || f == out.fallen->end() || f->second.serial != placed->serial ||
        f->second.trees.size() != cell.fallen_points.size()) {
        return false;
    }
    for (usize k = 0; k < cell.fallen_points.size(); ++k) {
        if (f->second.trees[k].point != cell.fallen_points[k]) return false;
    }
    SceneFrame::Cell fresh;
    emit_scatter(scene, *placed, renderer, meshes, out, fresh, &f->second.trees, true); // counts its materials' users
    const usize count = cell.instances.size() - cell.fallen_first;
    if (fresh.instances.size() != count || !fresh.decals.empty()) {
        for (const render::Instance& i : fresh.instances) {
            if (i.material < out.users.size()) --out.users[i.material]; // not used after all
        }
        return false;
    }
    for (usize k = 0; k < count; ++k) {
        render::Instance& was = cell.instances[cell.fallen_first + k];
        if (was.material < out.users.size() && --out.users[was.material] == 0) compact = true;
        was = fresh.instances[k];
        out.instances[cell.first + cell.fallen_first + k] = fresh.instances[k];
    }
    cell.prev_version = cell.version;
    cell.version = new_version();
    cell.changed_first = cell.fallen_first;
    cell.changed_count = static_cast<u32>(count);
    cell.fallen_dirty = false;
    ++out.cells_rebuilt;
    return true;
}

// Every entity (and built terrain tile) into its cell, all of them to be
// built; an empty material table.
void start_over(const world::Scene& scene, const TerrainMeshes* terrain, const FarGround* far,
                const ScatterPlacer* scatter, SceneFrame& out) {
    out.grid.clear();
    out.where.clear();
    out.lookup.clear();
    out.materials.clear();
    out.users.clear();
    out.order.clear();
    out.instances.clear(); // laid out afresh
    out.free.clear();
    out.pieces.clear();
    for (const world::Entity& e : scene.entities()) {
        const world::CellCoord c = world::cell_of(e.transform.position);
        out.grid[c].ids.push_back(e.id);
        out.where[e.id] = c;
    }
    out.terrain_material = scene.terrain.material;
    if (terrain != nullptr) {
        for (const auto& [c, heights] : scene.terrain.tiles()) {
            if (terrain->mesh(c) != render::k_no_mesh) out.grid[c];
        }
        if (far != nullptr) {
            for (const world::CellCoord c : far->piece_cells()) out.grid[c];
        }
    }
    if (scatter != nullptr) {
        for (const world::CellCoord c : scatter->cells()) out.grid[c];
    }
}

const render::Instance k_hole{{}, {}, {1.0f, 1.0f, 1.0f}, render::k_no_mesh, 0};

// Makes [first, first + count) a hole. For finding room it joins the
// holes beside it; it is sent as pieces of their own (k_piece at most),
// which keep their place and version until a cell takes them, so the
// renderer looks at a hole once, when it is made, and a cell taking part
// of one re-sends at most a piece.
constexpr u32 k_piece = 1024;
void release(SceneFrame& out, u32 first, u32 count) {
    if (count == 0) return;
    std::fill_n(out.instances.begin() + first, count, k_hole);
    for (u32 at = 0; at < count; at += k_piece) {
        out.pieces[first + at] = {std::min(k_piece, count - at), new_version()};
    }
    auto next = out.free.lower_bound(first);
    if (next != out.free.end() && next->first == first + count) {
        count += next->second.count;
        next = out.free.erase(next);
    }
    if (next != out.free.begin()) {
        const auto before = std::prev(next);
        if (before->first + before->second.count == first) {
            first = before->first;
            count += before->second.count;
            out.free.erase(before);
        }
    }
    out.free[first] = {count, 0};
}

// [first, first + count), part of a hole, taken by a cell: the pieces in
// it go, and one cut by its end leaves the rest as a new piece.
void take(SceneFrame& out, u32 first, u32 count) {
    const u32 end = first + count;
    auto it = out.pieces.lower_bound(first);
    while (it != out.pieces.end() && it->first < end) {
        const u32 piece_end = it->first + it->second.count;
        it = out.pieces.erase(it);
        if (piece_end > end) {
            out.pieces[end] = {piece_end - end, new_version()};
            break;
        }
    }
}

// Puts the cell's instances in its place, finding it a new one (the first
// hole it fits, or the end) if it has none or has outgrown it: with room to
// grow if it outgrew its place (a new cell gets none, so it fits a hole
// another cell of its size left).
void place(SceneFrame& out, SceneFrame::Cell& cell) {
    const u32 need = static_cast<u32>(cell.instances.size());
    // A place far bigger than the cell needs now (scatter thinned) is given
    // back, so its room is a hole others take.
    const bool shrink = cell.placed && cell.capacity > 4096 && need < cell.capacity / 4;
    if (!cell.placed || need > cell.capacity || shrink) {
        const bool room = cell.placed;
        if (cell.placed) release(out, cell.first, cell.capacity);
        cell.placed = true;
        cell.capacity = need == 0 || !room ? need : need + std::max<u32>(need / 4, 4);
        cell.first = static_cast<u32>(out.instances.size());
        for (auto it = out.free.begin(); it != out.free.end() && cell.capacity > 0; ++it) {
            if (it->second.count < cell.capacity) continue;
            cell.first = it->first;
            const u32 rest = it->second.count - cell.capacity;
            out.free.erase(it);
            if (rest > 0) out.free[cell.first + cell.capacity] = {rest, 0};
            take(out, cell.first, cell.capacity);
            break;
        }
        if (cell.capacity == 0) {
            cell.first = 0; // an empty place tiles anywhere, and holes never merge around it there
        } else if (cell.first == out.instances.size()) {
            out.instances.resize(out.instances.size() + cell.capacity, k_hole);
        }
    }
    std::copy(cell.instances.begin(), cell.instances.end(), out.instances.begin() + cell.first);
    std::fill_n(out.instances.begin() + cell.first + need, cell.capacity - need, k_hole);
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
            p.material.sway != q.material.sway || p.material.flutter != q.material.flutter ||
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
                 SceneFrame& out, TerrainMeshes* terrain, FarGround* far, ScatterPlacer* scatter,
                 const std::unordered_map<world::EntityId, world::Transform>* poses,
                 std::span<const world::EntityId> redraw,
                 const std::unordered_map<world::EntityId, std::vector<sim::PiecePose>>* piece_poses,
                 const sim::FallenTrees* fallen, std::span<const world::CellCoord> redraw_cells,
                 const std::unordered_map<world::EntityId, render::MeshId>* skinned) {
    out.cells_rebuilt = 0;
    out.poses = poses;
    out.skinned = skinned;
    out.piece_poses = piece_poses;
    out.fallen = fallen;
    world::SceneChanges changes = scene.take_changes();
    for (world::SceneChanges* follower : {&out.for_followers, &out.for_animation}) {
        if (follower->all) continue;
        if (changes.all || follower->ids.size() + changes.ids.size() > (usize{1} << 20)) {
            follower->ids.clear();
            follower->ids.shrink_to_fit();
            follower->all = true;
        } else {
            follower->ids.insert(follower->ids.end(), changes.ids.begin(), changes.ids.end());
        }
    }
    const std::vector<world::CellCoord> ground = far != nullptr       ? far->take_changed()
                                                 : terrain != nullptr ? terrain->take_changed()
                                                                      : std::vector<world::CellCoord>{};
    if (changes.all || scene.identity() != out.scene_identity || meshes.version() != out.meshes_version) {
        out.scene_identity = scene.identity();
        out.meshes_version = meshes.version();
        out.kits = scene.kits;
        start_over(scene, terrain, far, scatter, out);
        if (scatter != nullptr) scatter->take_changed(); // all built anyway
    } else {
        // Terrain tiles whose mesh came, went or changed; all of them for a new look.
        for (const world::CellCoord c : ground) {
            out.grid[c].ground_dirty = true; // (a new cell is dirty anyway)
        }
        if (!(scene.terrain.material == out.terrain_material)) {
            out.terrain_material = scene.terrain.material;
            for (auto& [coord, cell] : out.grid) cell.ground_dirty = cell.ground_dirty || cell.ground;
        }
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
        // Drawn somewhere else now (a pose moved, or went).
        for (const world::EntityId id : redraw) {
            const auto was = out.where.find(id);
            if (was != out.where.end()) out.grid[was->second].dirty = true;
        }
        // Fallen trees moved, came or went.
        for (const world::CellCoord c : redraw_cells) {
            const auto it = out.grid.find(c);
            if (it != out.grid.end() && it->second.scattered) it->second.fallen_dirty = true;
        }
        // Scatter placements changed.
        if (scatter != nullptr) {
            for (const world::CellCoord c : scatter->take_changed()) out.grid[c].dirty = true;
        }
        // Kits changed: the cells placing them (entities or scatter items).
        const std::vector<std::string> kits = changed_kits(out.kits, scene.kits);
        if (!kits.empty()) {
            out.kits = scene.kits;
            const auto scatters = [&](world::CellCoord coord) {
                const ScatterPlacer::Cell* placed = scatter == nullptr ? nullptr : scatter->find(coord);
                if (placed == nullptr) return false;
                for (const world::ScatterLayer& l : *placed->layers) {
                    for (const world::ScatterItem& i : l.items) {
                        if (i.shape == world::Shape::kit && std::find(kits.begin(), kits.end(), i.kit) != kits.end()) {
                            return true;
                        }
                    }
                }
                return false;
            };
            for (auto& [coord, cell] : out.grid) {
                cell.dirty = cell.dirty || (cell.scattered && scatters(coord));
                for (usize i = 0; i < cell.ids.size() && !cell.dirty; ++i) {
                    const world::Entity* e = scene.find(cell.ids[i]);
                    cell.dirty = e != nullptr && e->shape == world::Shape::kit &&
                                 std::find(kits.begin(), kits.end(), e->kit) != kits.end();
                }
            }
        }
    }
    // Room ahead for scatter (it holds up to max_instances), reserved once
    // when the first placements come: growing the list doubling as cells
    // come in would copy it all in one frame, again and again.
    if (scatter != nullptr && scatter->instances() > 0 && out.scatter_room == 0) {
        out.scatter_room = out.instances.size() + scatter->max_instances + scatter->max_instances / 4;
        out.instances.reserve(out.scatter_room);
    }
    // Rebuild what changed.
    bool layout = out.order.empty();
    bool compact = false;
    std::vector<world::CellCoord>& rebuilt = out.rebuilt;
    rebuilt.clear();
    u32 fallen_rebuilds = 0;
    for (auto it = out.grid.begin(); it != out.grid.end();) {
        SceneFrame::Cell& cell = it->second;
        bool in_place = false; // one change in place a frame (the renderer is told of one range)
        if (!cell.dirty && cell.ground_dirty) {
            // Only the ground: in place if it can be, else the whole cell.
            bool lost = false;
            cell.dirty = !rebuild_ground(scene, terrain, far, it->first, out, cell, lost);
            compact = compact || lost;
            in_place = !cell.dirty;
        }
        if (!cell.dirty && cell.fallen_dirty) {
            // Only fallen trees moved: in place if it can be, else the whole
            // cell (so many a frame; the rest stay waiting).
            bool lost = false;
            if (in_place || !rebuild_fallen(scene, renderer, meshes, scatter, it->first, out, cell, lost)) {
                if (fallen_rebuilds < out.fallen_rebuilds_per_frame) {
                    cell.dirty = true;
                    ++fallen_rebuilds;
                }
            }
            compact = compact || lost;
        }
        if (cell.dirty) {
            const bool fresh = cell.version == 0; // not laid out yet, even with no instances
            compact = !rebuild(scene, renderer, meshes, terrain, far, scatter, it->first, out, cell) || compact;
            layout = layout || fresh;
            rebuilt.push_back(it->first);
        }
        if (cell.ids.empty() && !cell.ground && !cell.scattered) {
            layout = true;
            if (cell.placed) release(out, cell.first, cell.capacity);
            it = out.grid.erase(it);
        } else {
            ++it;
        }
    }
    // Unused entries linger (a cell streaming out must not rebuild every
    // cell): the table is made again once they outnumber the used ones and
    // compact_unused_over.
    if (compact) {
        usize unused = 0;
        for (const u32 u : out.users) unused += u == 0 ? 1 : 0;
        compact = unused > out.compact_unused_over && unused > out.users.size() - unused;
    }
    if (compact) {
        start_over(scene, terrain, far, scatter, out);
        out.cells_rebuilt = 0; // counted once, by the full rebuild
        for (auto it = out.grid.begin(); it != out.grid.end();) {
            rebuild(scene, renderer, meshes, terrain, far, scatter, it->first, out, it->second);
            if (it->second.ids.empty() && !it->second.ground && !it->second.scattered) {
                it = out.grid.erase(it); // a tile gone since its mesh was built
            } else {
                ++it;
            }
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
    out.poses = nullptr; // the cells are built
    out.piece_poses = nullptr;
    out.skinned = nullptr;
    out.fallen = nullptr;
    if (!layout && out.cells_rebuilt == 0) {
        return; // as last frame
    }
    // Each cell keeps its place in the instance list (with room to grow),
    // so cells coming, going or changing leave the others where they are:
    // a cell that outgrows its place moves, and one that goes leaves a
    // hole (instances that draw nothing) for later cells to fill.
    bool all = (out.instances.empty() && out.free.empty()) || out.grid.empty(); // nothing to keep
    if (!all) {
        for (const world::CellCoord coord : rebuilt) {
            const auto it = out.grid.find(coord);
            if (it != out.grid.end()) place(out, it->second);
        }
        // Too much of the list is holes no cell has: laid out afresh, end
        // to end (each cell's own room to grow is not counted: it is at
        // most a quarter of it, and four).
        usize holes = 0;
        for (const auto& [first, block] : out.free) holes += block.count;
        all = holes > std::max(out.holes_allowed, out.instances.size() / 2) ||
              // holes push cells past what is drawn, and without them none would be
              (out.instances.size() > render::k_max_instances &&
               out.instances.size() - holes <= render::k_max_instances);
    }
    if (all) {
        out.instances.clear();
        out.free.clear();
        out.pieces.clear();
        for (const world::CellCoord coord : out.order) {
            SceneFrame::Cell& cell = out.grid.at(coord);
            cell.placed = false;
            place(out, cell);
        }
    }
    out.cells.clear();
    for (const auto& [coord, cell] : out.grid) {
        render::CellRange range{cell.first, cell.capacity, cell.version}; // empty ones too (decals only)
        if (cell.prev_version != 0) {
            // Changed from prev_version in its ground alone.
            range.prev_version = cell.prev_version;
            range.changed_first = cell.changed_first;
            range.changed_count = cell.changed_count;
        }
        out.cells.push_back(range);
    }
    for (const auto& [first, piece] : out.pieces) {
        out.cells.push_back({first, piece.count, piece.version});
    }
    std::sort(out.cells.begin(), out.cells.end(),
              [](const render::CellRange& a, const render::CellRange& b) {
                  return a.first != b.first ? a.first < b.first : a.count < b.count;
              });
    out.decals.clear();
    for (const world::CellCoord coord : out.order) {
        const SceneFrame::Cell& cell = out.grid.at(coord);
        out.decals.insert(out.decals.end(), cell.decals.begin(), cell.decals.end());
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
