#include "scene_render.h"

#include "far_ground.h"
#include "scatter_placer.h"
#include "texture_library.h"

#include "giant/sim/swing.h"

#include <algorithm>
#include <array>
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

// The textures a material's maps ask for (requested from the library),
// k_no_ref each for none. `textured`: drawn on something with uvs (built-in
// shapes, meshes whose file has them); else it draws the plain values.
// Looked up once a scene_frame call for each material's maps (a kit piece
// placed ten thousand times asks once).
std::array<u32, 4> texture_refs(const world::Material& m, bool textured, SceneFrame& out) {
    std::array<u32, 4> refs{TextureLibrary::k_no_ref, TextureLibrary::k_no_ref, TextureLibrary::k_no_ref,
                            TextureLibrary::k_no_ref};
    if (out.textures == nullptr || !textured || m.maps.empty()) return refs;
    const auto [it, added] = out.maps_seen.try_emplace(&m.maps);
    if (added) it->second = out.textures->request(m.maps);
    return it->second;
}

// Whether a shape (and its file, for a mesh) has uvs to draw maps with.
bool has_uvs(world::Shape shape, const MeshLibrary::Mesh* mesh) {
    switch (shape) {
    case world::Shape::cube:
    case world::Shape::sphere:
    case world::Shape::plane: return true;
    case world::Shape::mesh: return mesh != nullptr && mesh->has_uvs;
    default: return false;
    }
}

// `alpha_cutoff`: the material's own (the row's may be 0 while unused).
void set_textures(render::Material& r, const std::array<u32, 4>& refs, const TextureLibrary& textures,
                  float alpha_cutoff) {
    r.base_color_texture = textures.id(refs[0]);
    r.normal_texture = textures.id(refs[1]);
    r.roughness_metal_texture = textures.id(refs[2]);
    r.occlusion_texture = textures.id(refs[3]);
    // The voxel world's view of the base colour texture, while it is drawn.
    const TextureLibrary::Entry* e = r.base_color_texture != render::k_no_texture ? textures.entry(refs[0]) : nullptr;
    r.base_color_average = e != nullptr ? e->summary.average_at(alpha_cutoff) : Vec3{1.0f, 1.0f, 1.0f};
    r.coverage = e != nullptr ? e->summary.coverage_at(alpha_cutoff) : 1.0f;
}

// The material's index in the table, adding it if new, and one more user.
// `terrain`: the ground's, shaded from the frame's terrain layers.
u32 material_index(const world::Material& m, SceneFrame& out, bool textured = false, bool terrain = false) {
    SceneFrame::MaterialKey key{};
    const float values[10] = {m.base_color.x, m.base_color.y, m.base_color.z, m.roughness, m.metallic,
                              m.emissive.x,   m.emissive.y,   m.emissive.z,   m.sway,      m.flutter};
    std::memcpy(key.bits, values, sizeof(values));
    const std::array<u32, 4> refs = texture_refs(m, textured, out);
    std::copy(refs.begin(), refs.end(), key.bits + 10);
    key.bits[14] = terrain ? 1u : 0u;
    key.bits[15] = m.two_sided ? 1u : 0u;
    std::memcpy(&key.bits[16], &m.translucency, 4);
    std::memcpy(&key.bits[17], &m.alpha_cutoff, 4);
    std::memcpy(&key.bits[18], &m.canopy, 4);
    const auto [it, added] = out.lookup.try_emplace(key, static_cast<u32>(out.materials.size()));
    if (added) {
        render::Material r;
        r.base_color = m.base_color;
        r.roughness = m.roughness;
        r.metallic = m.metallic;
        r.emissive = m.emissive;
        r.sway = m.sway;
        r.flutter = m.flutter;
        r.terrain = terrain;
        r.two_sided = m.two_sided;
        r.translucency = m.translucency;
        r.alpha_cutoff = m.alpha_cutoff;
        r.canopy = m.canopy;
        if (out.textures != nullptr) set_textures(r, refs, *out.textures, m.alpha_cutoff);
        out.materials.push_back(r);
        out.material_textures.push_back(refs);
        out.material_kind.push_back({m.two_sided, m.alpha_cutoff});
        out.users.push_back(0);
    }
    ++out.users[it->second];
    return it->second;
}

// The ground's layers for the renderer, their textures requested (and
// their ids as ready now). Made again each frame: at most a few.
// Returns whether their textures changed (so the textures in use are told again).
bool terrain_layers(const world::Scene& scene, SceneFrame& out) {
    const std::vector<std::array<u32, 4>> before = std::move(out.terrain_layer_textures);
    out.terrain_layers.clear();
    out.terrain_layer_textures.clear();
    const std::vector<world::TerrainLayer>& layers = scene.terrain.material.layers;
    for (usize i = 0; i < std::min(layers.size(), world::k_max_terrain_layers); ++i) {
        const world::TerrainLayer& l = layers[i];
        render::TerrainLayer r;
        r.base_color = l.base_color;
        r.roughness = l.roughness;
        r.tiling = l.tiling;
        r.min_slope = l.min_slope;
        r.max_slope = l.max_slope;
        r.slope_blend = l.slope_blend;
        r.min_height = l.min_height;
        r.max_height = l.max_height;
        r.height_blend = l.height_blend;
        std::array<u32, 4> refs{TextureLibrary::k_no_ref, TextureLibrary::k_no_ref, TextureLibrary::k_no_ref,
                                TextureLibrary::k_no_ref};
        if (out.textures != nullptr && !l.maps.empty()) {
            world::MaterialMaps maps = l.maps;
            maps.occlusion.clear(); // (the ground has none)
            refs = out.textures->request(maps);
            r.base_color_texture = out.textures->id(refs[0]);
            r.normal_texture = out.textures->id(refs[1]);
            r.roughness_metal_texture = out.textures->id(refs[2]);
        }
        out.terrain_layers.push_back(r);
        out.terrain_layer_textures.push_back(refs);
    }
    return out.terrain_layer_textures != before;
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
          SceneFrame& out, SceneFrame::Cell& cell, render::MeshId own = render::k_no_mesh, bool moves = false) {
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
    render::Instance i;
    const MeshLibrary::Mesh* file = shape == world::Shape::mesh ? meshes.find_normalized(mesh_path) : nullptr;
    if (shape == world::Shape::cloth) {
        if (own == render::k_no_mesh) {
            return; // its sheet's mesh not made or loaded yet
        }
        mesh = own; // sized by its own settings: the entity's scale is unused (stays 1)
    } else if (shape == world::Shape::mesh && own != render::k_no_mesh) {
        mesh = own; // an animated copy
    } else if (shape == world::Shape::mesh) {
        if (file == nullptr || file->id == render::k_no_mesh) {
            return;
        }
        mesh = file->id;
    } else if (shape == world::Shape::kit) {
        return; // kits do not nest
    } else {
        mesh = renderer.builtin(builtin(shape));
    }
    if (shape != world::Shape::cloth) i.scale = t.scale;
    i.position = t.position;
    i.rotation = t.rotation;
    i.mesh = mesh;
    i.material = material_index(m, out, has_uvs(shape, file));
    i.moves = moves;
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
                        const MeshLibrary::Mesh* found = nullptr;
                        if (piece.shape == world::Shape::mesh) {
                            found = meshes.find_normalized(piece.mesh);
                            if (found != nullptr) lp.mesh = found->id;
                        } else if (piece.shape != world::Shape::decal) {
                            lp.mesh = renderer.builtin(builtin(piece.shape));
                        }
                        if (lp.mesh != render::k_no_mesh) {
                            lp.material = material_index(piece.material, out, has_uvs(piece.shape, found));
                            --out.users[lp.material]; // counted per instance below
                        }
                    }
                }
            }
            const MeshLibrary::Mesh* found = item.shape == world::Shape::mesh ? meshes.find_normalized(item.mesh) : nullptr;
            if (item.shape == world::Shape::mesh) {
                if (found != nullptr) look.mesh = found->id;
            } else if (item.shape != world::Shape::decal && item.shape != world::Shape::kit) {
                look.mesh = renderer.builtin(builtin(item.shape));
            }
            if (look.mesh != render::k_no_mesh) {
                look.material = material_index(item.material, out, has_uvs(item.shape, found));
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
        i.material = material_index(m, out, false, !scene.terrain.material.layers.empty());
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

// Where an entity is drawn: the physics' pose, else its walker's, else its
// swing's, else where the scene has it.
const world::Transform& drawn_at(const SceneFrame& out, const world::Entity& e) {
    if (out.poses != nullptr) {
        const auto p = out.poses->find(e.id);
        if (p != out.poses->end()) return p->second;
    }
    if (out.walkers != nullptr) {
        const auto p = out.walkers->find(e.id);
        if (p != out.walkers->end()) return p->second;
    }
    if (out.swings != nullptr) {
        const auto p = out.swings->find(e.id);
        if (p != out.swings->end()) return p->second;
    }
    return e.transform;
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
    cell.slots.clear();
    cell.moved.clear();
    std::sort(cell.ids.begin(), cell.ids.end());
    for (const world::EntityId id : cell.ids) {
        const world::Entity* e = scene.find(id);
        if (e == nullptr || !e->visible) {
            continue;
        }
        const world::Transform& at = drawn_at(out, *e);
        // Swinging, or walking.
        const bool moves = (e->swing.on && sim::can_swing(e->shape)) || (out.walkers != nullptr && out.walkers->contains(id));
        // What it draws, for moving it in place later (rebuild_moved).
        const u32 first = static_cast<u32>(cell.instances.size());
        const usize decals = cell.decals.size();
        const auto slot = [&] {
            cell.slots.push_back({id, first, static_cast<u32>(cell.instances.size()) - first, static_cast<u32>(decals),
                                  static_cast<u32>(cell.decals.size() - decals)});
        };
        if (e->shape != world::Shape::kit) {
            render::MeshId own = render::k_no_mesh;
            if (out.skinned != nullptr && e->shape == world::Shape::mesh) {
                const auto s = out.skinned->find(id);
                if (s != out.skinned->end()) own = s->second;
            }
            if (out.cloth != nullptr && e->shape == world::Shape::cloth) {
                const auto c = out.cloth->find(id);
                if (c != out.cloth->end()) own = c->second;
            }
            emit(e->shape, e->mesh, at, e->material, e->decal, renderer, meshes, out, cell, own, moves);
            slot();
            continue;
        }
        const world::Kit* kit = scene.find_kit(e->kit);
        if (kit == nullptr) {
            slot();
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
                 meshes, out, cell, render::k_no_mesh, moves);
        }
        slot();
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

// Moves a cell's entities in `cell.moved` (only where they are drawn
// changed: a pose, a swing) in place, the rest of the cell kept, its place
// too; false when it needs a full rebuild (one draws other than it did).
// `in_place`: the cell already changed in place this frame (its ground,
// its fallen trees): the range the renderer is told of grows to hold both.
bool rebuild_moved(const world::Scene& scene, SceneFrame& out, SceneFrame::Cell& cell, bool in_place) {
    if (cell.version == 0 || !cell.placed) {
        return false;
    }
    u32 lo = ~u32{0};
    u32 hi = 0;
    bool decals_moved = false;
    const auto put = [](render::Instance& i, const world::Transform& t, bool scaled) {
        i.position = t.position;
        i.rotation = t.rotation;
        if (scaled) i.scale = t.scale;
    };
    const auto put_decal = [](render::Decal& d, const world::Transform& t) {
        d.position = t.position;
        d.rotation = t.rotation;
        d.size = t.scale;
    };
    for (const world::EntityId id : cell.moved) {
        const auto slot = std::lower_bound(cell.slots.begin(), cell.slots.end(), id,
                                           [](const SceneFrame::Slot& s, world::EntityId v) { return s.id < v; });
        const world::Entity* e = scene.find(id);
        if (slot == cell.slots.end() || slot->id != id) {
            if (e == nullptr || !e->visible) continue; // drew nothing, draws nothing
            return false;
        }
        if (e == nullptr || !e->visible || slot->first + slot->count > cell.instances.size() ||
            slot->decal_first + slot->decals > cell.decals.size()) {
            return false;
        }
        const world::Transform& at = drawn_at(out, *e);
        if (e->shape == world::Shape::kit) {
            const world::Kit* kit = scene.find_kit(e->kit);
            if (kit == nullptr) {
                if (slot->count == 0 && slot->decals == 0) continue;
                return false;
            }
            // Each piece drew one instance or one decal, in order (else some
            // draw nothing: rebuilt whole); loose ones where they fell.
            u32 decal_pieces = 0;
            for (const world::KitPiece& p : kit->pieces) decal_pieces += p.shape == world::Shape::decal ? 1u : 0u;
            if (slot->decals != decal_pieces || slot->count + slot->decals != kit->pieces.size()) return false;
            const std::vector<sim::PiecePose>* loose = nullptr;
            if (out.piece_poses != nullptr) {
                const auto l = out.piece_poses->find(id);
                if (l != out.piece_poses->end()) loose = &l->second;
            }
            usize next = 0;
            u32 instance = slot->first;
            u32 decal = slot->decal_first;
            for (u32 k = 0; k < kit->pieces.size(); ++k) {
                const world::KitPiece& p = kit->pieces[k];
                while (loose != nullptr && next < loose->size() && (*loose)[next].piece < k) ++next;
                const bool fell = loose != nullptr && next < loose->size() && (*loose)[next].piece == k;
                const world::Transform t = fell ? (*loose)[next].at : place(at, p.transform);
                if (p.shape == world::Shape::decal) {
                    put_decal(cell.decals[decal++], t);
                } else {
                    put(cell.instances[instance++], t, true);
                }
            }
        } else {
            if (slot->count + slot->decals == 0) continue; // draws nothing (a mesh not loaded, a cloth with no mesh yet)
            if (slot->count + slot->decals != 1) return false;
            if (slot->decals == 1) {
                put_decal(cell.decals[slot->decal_first], at);
            } else {
                put(cell.instances[slot->first], at, e->shape != world::Shape::cloth);
            }
        }
        decals_moved = decals_moved || slot->decals > 0;
        if (slot->count > 0) {
            lo = std::min(lo, slot->first);
            hi = std::max(hi, slot->first + slot->count);
        }
    }
    cell.moved.clear();
    if (decals_moved) ++out.cells_rebuilt; // the frame's decals gathered again
    if (lo >= hi) {
        return true; // no instance moved
    }
    for (u32 k = lo; k < hi; ++k) out.instances[cell.first + k] = cell.instances[k];
    if (in_place && cell.prev_version != 0) {
        // Changed from the same version as the other in-place change: one range holding both.
        const u32 end = std::max(hi, cell.changed_first + cell.changed_count);
        cell.changed_first = std::min(lo, cell.changed_first);
        cell.changed_count = end - cell.changed_first;
    } else {
        cell.prev_version = cell.version;
        cell.version = new_version();
        cell.changed_first = lo;
        cell.changed_count = hi - lo;
    }
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
    out.material_textures.clear();
    out.material_kind.clear();
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
            p.material.two_sided != q.material.two_sided || p.material.translucency != q.material.translucency ||
            p.material.alpha_cutoff != q.material.alpha_cutoff || p.material.canopy != q.material.canopy ||
            !(p.material.maps == q.material.maps) ||
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
                 const std::unordered_map<world::EntityId, render::MeshId>* skinned,
                 const std::unordered_map<world::EntityId, render::MeshId>* cloth,
                 std::span<const world::EntityId> moved,
                 const std::unordered_map<world::EntityId, world::Transform>* swings,
                 const std::unordered_map<world::EntityId, world::Transform>* walkers) {
    out.cells_rebuilt = 0;
    out.maps_seen.clear(); // the scene may have changed since: its materials' addresses with it
    out.poses = poses;
    out.swings = swings;
    out.walkers = walkers;
    out.skinned = skinned;
    out.cloth = cloth;
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
        // (Only what the ground's instances carry: the layers themselves go
        // to the renderer each frame, so dragging a layer's slider
        // rebuilds no cells.)
        const world::TerrainMaterial& look = scene.terrain.material;
        if (!(look.base_color == out.terrain_material.base_color) || look.roughness != out.terrain_material.roughness ||
            look.layers.empty() != out.terrain_material.layers.empty()) {
            for (auto& [coord, cell] : out.grid) cell.ground_dirty = cell.ground_dirty || cell.ground;
        }
        out.terrain_material = look;
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
        // Drawn somewhere else now, and nothing else about them changed:
        // moved in place where they can be.
        for (const world::EntityId id : moved) {
            const auto was = out.where.find(id);
            if (was != out.where.end()) {
                SceneFrame::Cell& c = out.grid[was->second];
                if (!c.dirty) c.moved.push_back(id);
            }
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
            } else {
                in_place = true;
            }
            compact = compact || lost;
        }
        if (!cell.dirty && !cell.moved.empty()) {
            // Only where things are drawn: in place if it can be, else the
            // whole cell.
            cell.dirty = !rebuild_moved(scene, out, cell, in_place);
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
    out.swings = nullptr;
    out.walkers = nullptr;
    out.piece_poses = nullptr;
    out.skinned = nullptr;
    out.cloth = nullptr;
    out.fallen = nullptr;
    const bool layer_textures = terrain_layers(scene, out);
    // The textures drawn materials use, when that may have changed: the
    // rest leave the GPU after a while.
    if (out.textures != nullptr && (!rebuilt.empty() || compact || layer_textures)) {
        std::vector<u32>& used = out.scratch;
        used.clear();
        for (usize i = 0; i < out.materials.size(); ++i) {
            if (out.users[i] == 0) continue;
            for (const u32 ref : out.material_textures[i]) {
                if (ref != TextureLibrary::k_no_ref) used.push_back(ref);
            }
        }
        for (const std::array<u32, 4>& refs : out.terrain_layer_textures) {
            for (const u32 ref : refs) {
                if (ref != TextureLibrary::k_no_ref) used.push_back(ref);
            }
        }
        out.textures->set_used(used);
    }
    // Only materials in use are two-sided or cut out for the renderer (one
    // lingering unused would keep its kind's raster pass on; a few thousand
    // at most).
    for (usize i = 0; i < out.materials.size(); ++i) {
        const bool used = out.users[i] > 0;
        out.materials[i].two_sided = used && out.material_kind[i].two_sided;
        out.materials[i].alpha_cutoff = used ? out.material_kind[i].alpha_cutoff : 0.0f;
    }
    // Textures that came (or changed) since: every material asking for them
    // draws them from now on.
    if (out.textures != nullptr && out.textures->version() != out.textures_version) {
        out.textures_version = out.textures->version();
        for (usize i = 0; i < out.materials.size(); ++i) {
            set_textures(out.materials[i], out.material_textures[i], *out.textures, out.material_kind[i].alpha_cutoff);
        }
    }
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

static_assert(world::k_paint_channels == render::k_terrain_paint_channels);

void TerrainPaintSync::update(world::Terrain& terrain, render::Renderer& renderer) {
    m_sent = 0;
    world::TerrainChanges changes = terrain.take_paint_changes();
    if (terrain.identity() != m_identity || terrain.tile_size() != m_tile_size || terrain.samples() != m_samples) {
        m_identity = terrain.identity();
        changes.all = true;
    }
    if (terrain.tile_size() != m_tile_size || terrain.samples() != m_samples) {
        m_tile_size = terrain.tile_size();
        m_samples = terrain.samples();
        renderer.set_terrain_paint_grid(m_tile_size, m_samples); // drops what it had
    }
    const auto send = [&](world::CellCoord c) {
        const world::Terrain::Paint* p = terrain.paint(c);
        renderer.set_terrain_paint(c.x, c.z, p != nullptr ? *p : world::Terrain::Paint{});
        ++m_sent;
    };
    if (changes.all) {
        renderer.clear_terrain_paint();
        for (const auto& [cell, paint] : terrain.paints()) send(cell);
    } else {
        for (const world::CellCoord c : changes.tiles) send(c);
    }
}

} // namespace giant::editor
