// Terrain in the editor: tile meshes, drawing by cell, and the commands.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"
#include "../src/terrain_brush_tool.h"
#include "../src/terrain_meshes.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <set>
#include <thread>

using namespace giant;
using namespace giant::editor;
using ai::Json;
using world::CellCoord;
using world::Terrain;

namespace {

Terrain::Heights hills_tile(const Terrain& t, CellCoord c, u32 seed = 1) {
    world::HillsSettings h;
    h.amplitude = 15.0f;
    h.size = 60.0f;
    h.seed = seed;
    const usize n = t.samples();
    Terrain::Heights out(n * n);
    for (usize j = 0; j < n; ++j) {
        for (usize i = 0; i < n; ++i) {
            out[j * n + i] = world::hills_height(h, c.x * 64.0 + static_cast<double>(i) * t.spacing(),
                                                 c.z * 64.0 + static_cast<double>(j) * t.spacing());
        }
    }
    return out;
}

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    TerrainMeshes terrain{nullptr};
    WorldStreaming world;
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        state.terrain_meshes = &terrain;
        state.world = &world;
        register_scene_commands(commands, state);
        register_terrain_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};

} // namespace

GIANT_TEST("terrain meshes: a tile's surface meets its neighbours' exactly") {
    Terrain t;
    t.set_samples(16);
    const CellCoord a{0, 0}, b{1, 0}, c{0, 1};
    t.set(a, hills_tile(t, a));
    t.set(b, hills_tile(t, b));
    t.set(c, hills_tile(t, c));
    const TileSurface sa = tile_surface(tile_heights(t, a));
    const TileSurface sb = tile_surface(tile_heights(t, b));
    const TileSurface sc = tile_surface(tile_heights(t, c));
    const u32 row = 17;
    GIANT_CHECK(sa.mesh.vertices.size() == row * row && sa.mesh.indices.size() == 16u * 16u * 6u);
    // a's far x edge is b's near one: same place (in world space) and normal.
    for (u32 j = 0; j < row; ++j) {
        const geometry::Vertex& va = sa.mesh.vertices[j * row + 16];
        const geometry::Vertex& vb = sb.mesh.vertices[j * row + 0];
        GIANT_CHECK(va.position.x == 64.0f && vb.position.x == 0.0f);
        GIANT_CHECK(va.position.y == vb.position.y && va.position.z == vb.position.z);
        GIANT_CHECK(va.normal == vb.normal);
        GIANT_CHECK(sa.edge[j * row + 16] == 1 && sb.edge[j * row] == 1);
    }
    for (u32 i = 0; i < row; ++i) {
        const geometry::Vertex& va = sa.mesh.vertices[16 * row + i];
        const geometry::Vertex& vc = sc.mesh.vertices[i];
        GIANT_CHECK(va.position.y == vc.position.y && va.normal == vc.normal);
    }
    GIANT_CHECK(sa.edge[row + 1] == 0);
    // The surface is height_at's: vertices and the middle of triangles.
    for (const auto& v : sa.mesh.vertices) {
        if (const auto h = t.height_at(v.position.x, v.position.z)) { // (64, 64) is in the missing (1, 1)
            GIANT_CHECK(std::fabs(*h - v.position.y) < 1e-4);
        }
    }
    GIANT_CHECK(sa.mesh.vertices[row + 1].normal.y > 0.0f); // faces up
    // Counter-clockwise from above: every triangle's normal points up.
    for (usize k = 0; k < sa.mesh.indices.size(); k += 3) {
        const Vec3 p0 = sa.mesh.vertices[sa.mesh.indices[k]].position;
        const Vec3 p1 = sa.mesh.vertices[sa.mesh.indices[k + 1]].position;
        const Vec3 p2 = sa.mesh.vertices[sa.mesh.indices[k + 2]].position;
        GIANT_CHECK(cross(p1 - p0, p2 - p0).y > 0.0f);
    }
}

GIANT_TEST("terrain meshes: edge vertices survive every level of detail") {
    Terrain t; // 64 samples: enough triangles for several levels
    const CellCoord a{3, -2};
    t.set(a, hills_tile(t, a));
    const TileSurface s = tile_surface(tile_heights(t, a));
    const geometry::ClusterMesh m = geometry::build_clusters(s.mesh, s.edge);
    GIANT_CHECK(m.levels >= 3);
    // The edge as a set of points; each level's clusters must keep all of
    // them (a neighbour drawn at another level meets the same points).
    std::set<std::pair<float, float>> edge;
    for (usize v = 0; v < s.mesh.vertices.size(); ++v) {
        if (s.edge[v] != 0) edge.insert({s.mesh.vertices[v].position.x, s.mesh.vertices[v].position.z});
    }
    GIANT_CHECK(edge.size() == 4u * 64u);
    for (u32 depth = 0; depth < m.levels; ++depth) {
        std::set<std::pair<float, float>> seen;
        for (const geometry::ClusterGroup& g : m.groups) {
            if (g.depth != depth) continue;
            for (u32 c = g.first_cluster; c < g.first_cluster + g.cluster_count; ++c) {
                const geometry::Cluster& cl = m.clusters[c];
                for (u32 k = 0; k < cl.triangle_count * 3; ++k) {
                    const Vec3 p = m.vertices[m.indices[cl.first_index + k]].position;
                    if (edge.contains({p.x, p.z})) seen.insert({p.x, p.z});
                }
            }
        }
        GIANT_CHECK(seen.size() == edge.size());
    }
    // Without locks the coarsest level drops edge points (what the lock is for).
    const geometry::ClusterMesh loose = geometry::build_clusters(s.mesh);
    usize coarse = 0;
    for (const geometry::Cluster& cl : loose.clusters) {
        if (loose.groups[cl.group].depth + 1 != loose.levels) continue;
        coarse += cl.triangle_count;
    }
    usize coarse_locked = 0;
    for (const geometry::Cluster& cl : m.clusters) {
        if (m.groups[cl.group].depth + 1 == m.levels) coarse_locked += cl.triangle_count;
    }
    GIANT_CHECK(coarse < coarse_locked);
}

GIANT_TEST("terrain meshes: built off the main thread, a change rebuilds its neighbours") {
    Terrain t;
    t.set_samples(8);
    for (i32 z = 0; z < 3; ++z) {
        for (i32 x = 0; x < 3; ++x) t.set({x, z}, Terrain::Heights(64, 1.0f));
    }
    TerrainMeshes m(nullptr);
    m.update(t);
    GIANT_CHECK(m.stats().building == 9 && m.stats().tiles == 0);
    m.finish(t);
    GIANT_CHECK(m.stats().tiles == 9 && m.stats().building == 0 && m.stats().built == 9);
    GIANT_CHECK(m.stats().triangles == 9u * 8u * 8u * 2u);
    GIANT_CHECK(m.take_changed().size() == 9 && m.take_changed().empty());

    // A sample inside a tile: only that tile's mesh reads it.
    t.modify({0, 0})->at(0) = 4.0f;
    m.finish(t);
    GIANT_CHECK(m.stats().built == 10);
    GIANT_CHECK((m.take_changed() == std::vector<CellCoord>{CellCoord{0, 0}}));
    // Its far corner sample: the three tiles around that corner read it too.
    t.modify({0, 0})->at(63) = 4.0f;
    m.finish(t);
    GIANT_CHECK(m.stats().built == 14);
    std::vector<CellCoord> changed = m.take_changed();
    GIANT_CHECK(changed.size() == 4);
    GIANT_CHECK(std::find(changed.begin(), changed.end(), CellCoord{1, 1}) != changed.end());
    GIANT_CHECK(std::find(changed.begin(), changed.end(), CellCoord{2, 0}) == changed.end());

    // Removed: its mesh goes, and neighbours that read it rebuild.
    t.remove({1, 1});
    m.finish(t);
    GIANT_CHECK(m.stats().tiles == 8 && m.stats().building == 0 && m.stats().built > 14);
    changed = m.take_changed();
    GIANT_CHECK(std::find(changed.begin(), changed.end(), CellCoord{1, 1}) != changed.end());

    // A copy (a scene put back by undo, say) with the same heights builds nothing.
    const u64 built = m.stats().built;
    Terrain fresh = t;
    m.update(fresh);
    GIANT_CHECK(m.stats().building == 0 && m.stats().in_flight == 0);
    m.finish(fresh);
    GIANT_CHECK(m.stats().built == built && m.take_changed().empty());

    // Builds are budgeted per frame and in flight; uploads per frame.
    for (auto& [c, h] : fresh.tiles()) fresh.modify(c)->at(9) = 2.0f;
    m.builds_per_frame = 2;
    m.update(fresh);
    GIANT_CHECK(m.stats().in_flight == 2 && m.stats().building == 8);
    m.builds_per_frame = 8;
    m.builds_in_flight = 3;
    m.update(fresh);
    GIANT_CHECK(m.stats().in_flight <= 3);
    m.builds_in_flight = 8;
    m.uploads_per_frame = 1;
    for (int frame = 0; frame < 20000 && m.stats().building > 0; ++frame) {
        const u64 before = m.stats().built;
        m.update(fresh);
        GIANT_CHECK(m.stats().built - before <= 1);
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // a frame's worth of time for the workers
    }
    GIANT_CHECK(m.stats().building == 0 && m.stats().built == built + 8);
}

GIANT_TEST("terrain meshes: new tiles settle before building; changed ones do not wait; drops are budgeted") {
    Terrain t;
    t.set_samples(8);
    TerrainMeshes m(nullptr);
    m.settle_frames = 3;
    m.update(t); // a terrain new to it: everything is looked at at once
    // A row of tiles arriving one a frame (a world streaming in): each builds once.
    for (i32 x = 0; x < 6; ++x) {
        t.set({x, 0}, Terrain::Heights(64, 1.0f));
        m.update(t);
    }
    GIANT_CHECK(m.stats().in_flight <= 2); // the first ones, whose neighbours are all in
    for (int frame = 0; frame < 20000 && m.stats().building > 0; ++frame) {
        m.update(t);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    GIANT_CHECK(m.stats().tiles == 6 && m.stats().built == 6);
    // A change to a tile already there starts at once.
    t.modify({2, 0})->at(10) = 3.0f;
    m.update(t);
    GIANT_CHECK(m.stats().in_flight == 1);
    m.finish(t);
    GIANT_CHECK(m.stats().built == 7);
    // Many tiles going at once are dropped a few a frame (drops_per_frame), builds or not.
    for (i32 z = 1; z < 11; ++z) {
        for (i32 x = 0; x < 10; ++x) t.set({x, z}, Terrain::Heights(64, 1.0f));
    }
    m.finish(t);
    GIANT_CHECK(m.stats().tiles == 106);
    m.drops_per_frame = 16;
    for (i32 z = 1; z < 11; ++z) {
        for (i32 x = 0; x < 10; ++x) t.remove({x, z});
    }
    m.update(t);
    GIANT_CHECK(m.stats().tiles == 106 - 16);
    m.finish(t);
    GIANT_CHECK(m.stats().tiles == 6);
}

GIANT_TEST("terrain meshes: a tile changed while it builds is drawn with its newest heights") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    if (renderer == nullptr) return;
    Terrain t;
    t.set_samples(8);
    t.set({0, 0}, Terrain::Heights(64, 0.0f));
    TerrainMeshes m(renderer.get());
    m.finish(t);
    const render::MeshId first = m.mesh({0, 0});
    GIANT_CHECK(first != render::k_no_mesh);
    t.modify({0, 0})->at(9) = 5.0f;
    m.update(t); // builds with 5
    t.modify({0, 0})->at(9) = 7.0f;
    m.finish(t);
    const geometry::ClusterMesh* drawn = renderer->mesh(m.mesh({0, 0}));
    GIANT_CHECK(drawn != nullptr);
    float highest = 0.0f;
    for (const geometry::Vertex& v : drawn->vertices) highest = std::max(highest, v.position.y);
    GIANT_CHECK(highest == 7.0f);
    // Old meshes are removed, and their ids reused: rebuilding does not grow the renderer's table.
    for (int k = 0; k < 20; ++k) {
        t.modify({0, 0})->at(9) = static_cast<float>(k);
        m.finish(t);
    }
    GIANT_CHECK(m.mesh({0, 0}) <= first + 2);
}

GIANT_TEST("terrain: tiles draw in their cells, as meshes at every distance") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    if (renderer == nullptr) return;
    MeshLibrary meshes(renderer.get());
    TerrainMeshes terrain(renderer.get());
    world::Scene scene;
    SceneFrame frame;
    scene.terrain.set_samples(8);
    scene.terrain.set({0, 0}, Terrain::Heights(64, 2.0f));
    scene.terrain.set({1, 0}, Terrain::Heights(64, 2.0f));
    world::Entity rock;
    rock.transform.position = {10.0, 2.0, 10.0};
    scene.add(rock);
    const auto build = [&] {
        terrain.update(scene.terrain);
        scene_frame(scene, *renderer, meshes, frame, &terrain);
    };
    const auto live = [&] {
        std::vector<render::Instance> out;
        for (const render::Instance& i : frame.instances) {
            if (i.mesh != render::k_no_mesh) out.push_back(i);
        }
        return out;
    };
    build(); // meshes not built yet: only the rock
    GIANT_CHECK(live().size() == 1 && !live()[0].always_mesh);
    terrain.finish(scene.terrain);
    scene_frame(scene, *renderer, meshes, frame, &terrain);
    std::vector<render::Instance> now = live();
    GIANT_CHECK(now.size() == 3);
    usize ground = 0;
    for (const render::Instance& i : now) {
        if (!i.always_mesh) continue;
        ++ground;
        GIANT_CHECK(i.mesh == terrain.mesh(world::cell_of(i.position)) && i.position.y == 0.0);
        GIANT_CHECK(frame.materials[i.material].base_color == scene.terrain.material.base_color);
    }
    GIANT_CHECK(ground == 2 && frame.cells_rebuilt == 2); // the rock's cell and the empty one
    // The ground comes first in its cell.
    for (const render::CellRange& r : frame.cells) {
        for (u32 k = 1; k < r.count; ++k) GIANT_CHECK(!frame.instances[r.first + k].always_mesh);
    }

    // Nothing changed: nothing rebuilt.
    build();
    GIANT_CHECK(frame.cells_rebuilt == 0);
    // A new look: only the cells with ground, and in them only the ground
    // (in place: the rock is as it was, and the renderer is told so).
    const std::vector<render::CellRange> cells_before = frame.cells;
    const std::vector<render::Instance> list_before = frame.instances;
    scene.terrain.material.base_color = {0.5f, 0.1f, 0.1f};
    build();
    GIANT_CHECK(frame.cells_rebuilt == 2);
    for (const render::Instance& i : live()) {
        if (i.always_mesh) GIANT_CHECK(frame.materials[i.material].base_color == scene.terrain.material.base_color);
    }
    GIANT_CHECK(frame.cells.size() == cells_before.size() && frame.instances.size() == list_before.size());
    for (usize k = 0; k < frame.cells.size(); ++k) {
        const render::CellRange& r = frame.cells[k];
        GIANT_CHECK(r.first == cells_before[k].first && r.count == cells_before[k].count);
        if (r.version == cells_before[k].version) continue; // a hole
        GIANT_CHECK(r.prev_version == cells_before[k].version && r.changed_first == 0 && r.changed_count == 1);
        for (u32 j = 1; j < r.count; ++j) {
            const render::Instance& a = frame.instances[r.first + j];
            const render::Instance& b = list_before[r.first + j];
            GIANT_CHECK(a.position == b.position && a.mesh == b.mesh && a.material == b.material);
        }
    }
    // Layers: the ground's material turns terrain (shaded from the frame's
    // layers), the layers go to the renderer as given, and the plain
    // colour stays for the far voxel world.
    GIANT_CHECK(frame.terrain_layers.empty());
    world::TerrainLayer grass;
    grass.base_color = {0.1f, 0.3f, 0.05f};
    world::TerrainLayer cliff;
    cliff.min_slope = 35.0f;
    cliff.tiling = 8.0f;
    scene.terrain.material.layers = {grass, cliff};
    build();
    GIANT_CHECK(frame.cells_rebuilt == 2 && frame.terrain_layers.size() == 2);
    GIANT_CHECK(frame.terrain_layers[0].base_color == grass.base_color && frame.terrain_layers[1].min_slope == 35.0f &&
                frame.terrain_layers[1].tiling == 8.0f);
    GIANT_CHECK(frame.terrain_layers[0].base_color_texture == render::k_no_texture); // no images (and no library)
    for (const render::Instance& i : live()) {
        GIANT_CHECK(frame.materials[i.material].terrain == i.always_mesh); // the rock is not ground
        if (i.always_mesh) GIANT_CHECK(frame.materials[i.material].base_color == scene.terrain.material.base_color);
    }
    // A layer's values change: the renderer gets them, no cell is rebuilt.
    scene.terrain.material.layers[1].min_slope = 40.0f;
    build();
    GIANT_CHECK(frame.cells_rebuilt == 0 && frame.terrain_layers[1].min_slope == 40.0f);
    scene.terrain.material.layers.clear();
    build();
    GIANT_CHECK(frame.cells_rebuilt == 2 && frame.terrain_layers.empty());
    for (const render::Instance& i : live()) GIANT_CHECK(!frame.materials[i.material].terrain);
    // A tile goes: its cell (with no entities) goes once the mesh is dropped.
    scene.terrain.remove({1, 0});
    terrain.finish(scene.terrain);
    scene_frame(scene, *renderer, meshes, frame, &terrain);
    now = live();
    GIANT_CHECK(now.size() == 2);
    GIANT_CHECK(std::count_if(now.begin(), now.end(), [](const render::Instance& i) { return i.always_mesh; }) == 1);
    // A new scene: its tiles from scratch.
    world::Scene copy = scene;
    terrain.finish(copy.terrain);
    scene_frame(copy, *renderer, meshes, frame, &terrain);
    GIANT_CHECK(live().size() == 2);
}

GIANT_TEST("terrain commands: create, shape, sample, remove, all undoable") {
    Fixture f;
    // A 100 m square from (-10, -10): cells -1 to 1 on both axes.
    ai::CommandResult r = f.run("create_terrain", {{"from", {-10, -10}}, {"to", {90, 90}}, {"height", 5}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 9 && r.value["first_cell"] == Json::array({-1, -1}));
    GIANT_CHECK(r.value["heights"]["lowest"] == 5.0 && r.value["heights"]["highest"] == 5.0);
    // An edge on a cell border does not reach into the next cell.
    r = f.run("create_terrain", {{"from", {0, 0}}, {"to", {64, 128}}, {"hills", {{"amplitude", 10}, {"seed", 3}}}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 2 && r.value["terrain_tiles"] == 9); // (0, 0) and (0, 1): already there
    const float low = r.value["heights"]["lowest"].get<float>();
    const float high = r.value["heights"]["highest"].get<float>();
    GIANT_CHECK(low >= -10.0f && high <= 10.0f && high > low);

    r = f.run("get_terrain", {{"points", {{32, 32}, {-5, -5}, {500, 500}}}, {"cell", {-1, -1}}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 9 && r.value["samples"] == 64 && r.value["spacing"] == 1.0);
    GIANT_CHECK(r.value["heights"][1] == 5.0 && r.value["heights"][2].is_null());
    GIANT_CHECK(std::fabs(r.value["heights"][0].get<double>() - *f.state.scene.terrain.height_at(32, 32)) < 1e-9);
    GIANT_CHECK(r.value["cell_heights"].size() == 4096 && r.value["meshes"]["building"] == 9);
    GIANT_CHECK(r.value["first_cell"] == Json::array({-1, -1}) && r.value["last_cell"] == Json::array({1, 1}));

    // The spacing changes only with no terrain.
    r = f.run("create_terrain", {{"from", {0, 0}}, {"to", {1, 1}}, {"samples", 16}});
    GIANT_CHECK(!r.ok && r.error.find("only while there is no terrain") != std::string::npos);
    GIANT_CHECK(!f.run("create_terrain", {{"from", {0, 0}}, {"to", {1, 1}}, {"samples", 48}}).ok); // schema
    GIANT_CHECK(!f.run("create_terrain", {{"from", {0, 0}}, {"to", {5000, 5000}}}).ok);           // too many

    // One tile by hand.
    Json heights = Json::array();
    for (int i = 0; i < 4096; ++i) heights.push_back(i % 64 == 0 ? 3.0 : 1.0);
    r = f.run("set_terrain_heights", {{"cell", {7, 7}}, {"heights", heights}});
    GIANT_CHECK(r.ok && r.value["terrain_tiles"] == 10 && r.value["heights"]["highest"] == 3.0);
    GIANT_CHECK(!f.run("set_terrain_heights", {{"cell", {7, 7}}, {"heights", Json::array({1, 2, 3})}}).ok);
    r = f.run("set_terrain_heights", {{"cell", {7, 7}}, {"heights", Json(std::vector<float>(64, 1.0f))}});
    GIANT_CHECK(!r.ok && r.error.find("must hold 4096") != std::string::npos);

    r = f.run("set_terrain_material", {{"base_color", {0.1, 0.4, 0.1}}, {"roughness", 0.7}});
    GIANT_CHECK(r.ok && f.state.scene.terrain.material.roughness == 0.7f);

    // Undo walks it all back.
    GIANT_CHECK(f.commands.undo()); // material
    GIANT_CHECK(f.state.scene.terrain.material == world::TerrainMaterial{});
    GIANT_CHECK(f.commands.undo()); // the hand-made tile
    GIANT_CHECK(f.state.scene.terrain.size() == 9 && f.state.scene.terrain.find({7, 7}) == nullptr);
    GIANT_CHECK(f.commands.undo()); // the hills: those two tiles flat again
    GIANT_CHECK(*f.state.scene.terrain.height_at(32, 32) == 5.0 && f.state.scene.terrain.size() == 9);
    GIANT_CHECK(f.commands.redo());
    GIANT_CHECK(*f.state.scene.terrain.height_at(32, 32) != 5.0 && f.state.scene.terrain.size() == 9);

    // Removing: an area, then everything; undo brings them back.
    r = f.run("remove_terrain", {{"from", {-64, -64}}, {"to", {-1, -1}}});
    GIANT_CHECK(r.ok && r.value["removed"] == 1 && r.value["terrain_tiles"] == 8);
    r = f.run("remove_terrain", {{"all", true}});
    GIANT_CHECK(r.ok && r.value["removed"] == 8 && f.state.scene.terrain.empty());
    GIANT_CHECK(!f.run("remove_terrain").ok);
    // With none left, the spacing may change (and undo puts both back).
    r = f.run("create_terrain", {{"from", {0, 0}}, {"to", {10, 10}}, {"samples", 16}});
    GIANT_CHECK(r.ok && f.state.scene.terrain.samples() == 16 && r.value["spacing"] == 4.0);
    GIANT_CHECK(f.commands.undo());
    GIANT_CHECK(f.state.scene.terrain.empty() && f.state.scene.terrain.samples() == 64);
    GIANT_CHECK(f.commands.undo());
    GIANT_CHECK(f.state.scene.terrain.size() == 8);
    GIANT_CHECK(f.commands.redo() && f.commands.redo());
    GIANT_CHECK(f.state.scene.terrain.samples() == 16 && f.state.scene.terrain.size() == 1);
}

GIANT_TEST("terrain commands: layers set as a whole, read back, undone, and their images checked") {
    Fixture f;
    ai::CommandResult r = f.run("set_terrain_layers",
                                {{"layers", Json::array({Json{{"base_color", {0.1, 0.3, 0.05}}},
                                                         Json{{"min_slope", 35}, {"tiling", 8}, {"roughness", 0.8}},
                                                         Json{{"min_height", 1500}, {"height_blend", 50}}})}});
    GIANT_CHECK(r.ok && r.value["layers"].size() == 3);
    const std::vector<world::TerrainLayer>& layers = f.state.scene.terrain.material.layers;
    GIANT_CHECK(layers.size() == 3 && layers[0].base_color == (Vec3{0.1f, 0.3f, 0.05f}) && layers[1].min_slope == 35.0f &&
                layers[1].tiling == 8.0f && layers[1].roughness == 0.8f && layers[2].min_height == 1500.0f &&
                layers[2].height_blend == 50.0f && layers[2].max_slope == 90.0f); // left out: the defaults
    r = f.run("get_terrain");
    GIANT_CHECK(r.ok && r.value["material"]["layers"].size() == 3 && r.value["material"]["layers"][1]["min_slope"] == 35.0);
    // The plain look keeps the layers.
    GIANT_CHECK(f.run("set_terrain_material", {{"roughness", 0.5}}).ok && layers.size() == 3);
    GIANT_CHECK(f.commands.undo());
    // Out of range, too many, an image that is not there, occlusion: refused, nothing changed.
    GIANT_CHECK(!f.run("set_terrain_layers", {{"layers", Json::array({Json{{"max_slope", 95}}})}}).ok);
    GIANT_CHECK(!f.run("set_terrain_layers", {{"layers", Json::array({Json{{"tiling", 0}}})}}).ok);
    GIANT_CHECK(!f.run("set_terrain_layers", {{"layers", Json(std::vector<Json>(9, Json::object()))}}).ok);
    r = f.run("set_terrain_layers",
              {{"layers", Json::array({Json::object(), Json{{"maps", {{"base_color", "no/such/grass.png"}}}}})}});
    GIANT_CHECK(!r.ok && r.error.find("layers[1].maps.base_color") != std::string::npos);
    GIANT_CHECK(!f.run("set_terrain_layers", {{"layers", Json::array({Json{{"maps", {{"occlusion", "x.png"}}}}})}}).ok);
    GIANT_CHECK(layers.size() == 3);
    GIANT_CHECK(f.run("set_terrain_layers", {{"layers", Json::array({Json{{"tiling", 0.05}}})}}).ok); // the least
    GIANT_CHECK(f.commands.undo() && layers.size() == 3);
    // An image a layer names already goes back as it is, even with its file
    // gone (read back, one value changed, sent again).
    f.state.scene.terrain.material.layers[0].maps.base_color = "gone/grass.png";
    Json back = f.run("get_terrain").value["material"]["layers"];
    back[1]["roughness"] = 0.3;
    r = f.run("set_terrain_layers", {{"layers", back}});
    GIANT_CHECK(r.ok && layers[0].maps.base_color == "gone/grass.png" && layers[1].roughness == 0.3f);
    GIANT_CHECK(f.commands.undo());
    // None again, then undo, then redo.
    GIANT_CHECK(f.run("set_terrain_layers", {{"layers", Json::array()}}).ok && layers.empty());
    GIANT_CHECK(f.commands.undo() && layers.size() == 3);
    GIANT_CHECK(f.commands.undo() && layers.empty());
    GIANT_CHECK(f.commands.redo() && layers.size() == 3);
}

GIANT_TEST("terrain commands: work a command and a terrain may hold is capped") {
    Fixture f;
    // 40 x 40 tiles of 128 samples: 26M heights, over the 16.7M a command may make.
    ai::CommandResult r = f.run("create_terrain", {{"from", {0, 0}}, {"to", {2560, 2560}}, {"samples", 128}});
    GIANT_CHECK(!r.ok && r.error.find("split it") != std::string::npos && f.state.scene.terrain.empty());
    GIANT_CHECK(f.state.scene.terrain.samples() == 64); // refused: nothing changed
    // Under it: fine, and made in parallel the same as one by one.
    r = f.run("create_terrain", {{"from", {0, 0}}, {"to", {639, 639}}, {"samples", 128},
                                 {"hills", {{"seed", 9}}}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 100);
    world::HillsSettings h;
    h.seed = 9;
    GIANT_CHECK(f.state.scene.terrain.find({7, 3})->at(5 * 128 + 77) ==
                world::hills_height(h, 7 * 64.0 + 77 * 0.5, 3 * 64.0 + 5 * 0.5));
}

GIANT_TEST("terrain commands: saved with the scene") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {100, 10}}, {"hills", Json::object()}}).ok);

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "giant_terrain.scene.json";
    GIANT_CHECK(f.run("save_scene", {{"path", file.string()}}).ok);
    const world::Terrain made = f.state.scene.terrain;
    GIANT_CHECK(f.run("new_scene").ok && f.state.scene.terrain.empty());
    GIANT_CHECK(f.run("load_scene", {{"path", file.string()}}).ok);
    GIANT_CHECK(f.state.scene.terrain.size() == 2 && f.state.scene.terrain.tiles() == made.tiles());
    std::error_code e;
    std::filesystem::remove(file, e);
}

GIANT_TEST("terrain commands: sculpt a stroke, undone in one step; a drag's calls are one step too") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {127, 63}}, {"samples", 16}, {"height", 2}}).ok);
    ai::CommandResult r = f.run("sculpt_terrain", {{"op", "raise"}, {"points", {{20, 30}, {100, 30}}},
                                                   {"radius", 6}, {"strength", 1}, {"falloff", 0}});
    GIANT_CHECK(r.ok && r.value["tiles_changed"] == 2 && r.value["dabs"].get<int>() > 50);
    const Terrain& t = f.state.scene.terrain;
    GIANT_CHECK(t.height_at(60, 30).value() > 2.0 && t.height_at(60, 50).value() == 2.0);
    GIANT_CHECK(f.run("undo").ok && t.height_at(60, 30).value() == 2.0);
    GIANT_CHECK(f.run("redo").ok && t.height_at(60, 30).value() > 2.0);
    GIANT_CHECK(f.run("undo").ok);
    // Two calls in one merge group (a drag): one undo step.
    const Json dab = {{"op", "flatten"}, {"points", {{10, 10}}}, {"height", -3}, {"strength", 1}, {"falloff", 0}};
    GIANT_CHECK(f.commands.execute("sculpt_terrain", dab, {77, true}).ok);
    Json dab2 = dab;
    dab2["points"] = {{40, 10}};
    GIANT_CHECK(f.commands.execute("sculpt_terrain", dab2, {77, true}).ok);
    GIANT_CHECK(t.height_at(10, 10).value() == -3.0 && t.height_at(40, 10).value() == -3.0);
    GIANT_CHECK(f.run("undo").ok && t.height_at(10, 10).value() == 2.0 && t.height_at(40, 10).value() == 2.0);
    // Nothing there: nothing changes, no undo step; schema and reach checks.
    const usize steps = f.commands.undo_history().size();
    r = f.run("sculpt_terrain", {{"op", "lower"}, {"points", {{5000, 5000}}}});
    GIANT_CHECK(r.ok && r.value["tiles_changed"] == 0 && f.commands.undo_history().size() == steps);
    GIANT_CHECK(!f.run("sculpt_terrain", {{"op", "dig"}, {"points", {{0, 0}}}}).ok);
    GIANT_CHECK(!f.run("sculpt_terrain", {{"op", "raise"}, {"points", {{0, 0}, {100000, 0}}}, {"radius", 100}}).ok);
}

GIANT_TEST("terrain commands: a long drag keeps one copy per tile; undone mid-drag, it starts a new step") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {127, 63}}, {"samples", 16}, {"height", 0}}).ok);
    const Terrain& t = f.state.scene.terrain;
    const usize steps = f.commands.undo_history().size();
    Json dab = {{"op", "raise"}, {"points", {{30, 30}}}, {"radius", 6}, {"strength", 1}, {"falloff", 0}};
    for (int k = 0; k < 200; ++k) GIANT_CHECK(f.commands.execute("sculpt_terrain", dab, {91, true}).ok);
    dab["points"] = {{90, 30}};
    for (int k = 0; k < 50; ++k) GIANT_CHECK(f.commands.execute("sculpt_terrain", dab, {91, true}).ok);
    GIANT_CHECK(f.commands.undo_history().size() == steps + 1);
    GIANT_CHECK(t.height_at(30, 30).value() == 200.0 && t.height_at(90, 30).value() == 50.0);
    const Terrain::Heights end0 = *t.find({0, 0});
    const Terrain::Heights end1 = *t.find({1, 0});
    const u64 version0 = t.version({0, 0});
    GIANT_CHECK(f.run("undo").ok && t.height_at(30, 30).value() == 0.0 && t.height_at(90, 30).value() == 0.0);
    GIANT_CHECK(f.run("redo").ok && *t.find({0, 0}) == end0 && *t.find({1, 0}) == end1);
    GIANT_CHECK(t.version({0, 0}) == version0);
    // Undone in the middle of a drag: the drag's next calls are a new step.
    GIANT_CHECK(f.run("undo").ok);
    dab["points"] = {{30, 30}};
    GIANT_CHECK(f.commands.execute("sculpt_terrain", dab, {91, true}).ok);
    GIANT_CHECK(f.commands.execute("sculpt_terrain", dab, {91, true}).ok);
    GIANT_CHECK(t.height_at(30, 30).value() == 2.0 && f.commands.undo_history().size() == steps + 1);
    GIANT_CHECK(f.run("undo").ok && t.height_at(30, 30).value() == 0.0);
    GIANT_CHECK(f.commands.undo_history().size() == steps);
    // A dab that changes nothing makes no step; a stroke's work is capped.
    dab["strength"] = 0;
    GIANT_CHECK(f.run("sculpt_terrain", dab).value["tiles_changed"] == 0 && f.commands.undo_history().size() == steps);
    Json zigzag = {{"op", "raise"}, {"radius", 0.5}, {"points", Json::array()}};
    for (int k = 0; k < 4096; ++k) zigzag["points"].push_back({k % 2 == 0 ? 1.0 : 60.0, 30.0});
    GIANT_CHECK(!f.run("sculpt_terrain", zigzag).ok); // ~1.9 million dabs
    GIANT_CHECK(!f.run("sculpt_terrain", {{"op", "smooth"}, {"points", {{30, 30}, {3000, 30}}}, {"radius", 2000}}).ok);
}

GIANT_TEST("terrain commands: a height map ending on a cell's edge meets the tile past it") {
    Fixture f;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() /
        ("giant_height_edge_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".r16");
    {
        std::ofstream out(file, std::ios::binary);
        const char bytes[] = {'\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF'}; // all white
        out.write(bytes, sizeof(bytes));
    }
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {127, 63}}, {"samples", 16}, {"height", 7}}).ok);
    ai::CommandResult r = f.run("import_height_map", {{"file", file.generic_string()}, {"from", {0, 0}},
                                                      {"to", {64, 64}}, {"low", 0}, {"high", 20}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 2); // cell (0, 0) and the column x = 64 of cell (1, 0)
    const Terrain& t = f.state.scene.terrain;
    GIANT_CHECK(t.height_at(63.9, 10).value() > 19.9 && t.height_at(64.0, 10).value() == 20.0);
    GIANT_CHECK(t.height_at(68.0, 10).value() == 7.0); // past `to`: as it was
    GIANT_CHECK(t.find({0, 1}) == nullptr && t.find({1, 1}) == nullptr); // no tiles made past the edge
    GIANT_CHECK(f.run("undo").ok && t.height_at(64.0, 10).value() == 7.0 && t.size() == 2);
    std::filesystem::remove(file);
}

GIANT_TEST("terrain commands: a height map makes ground over an area, black at low and white at high") {
    Fixture f;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() /
        ("giant_height_map_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".r16");
    {
        // 2 x 2: black on the first row, white on the second.
        std::ofstream out(file, std::ios::binary);
        const char bytes[] = {0, 0, 0, 0, '\xFF', '\xFF', '\xFF', '\xFF'};
        out.write(bytes, sizeof(bytes));
    }
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {63, 63}}, {"samples", 16}, {"height", 7}}).ok);
    ai::CommandResult r = f.run("import_height_map", {{"file", file.generic_string()}, {"from", {0, 0}},
                                                      {"to", {100, 100}}, {"low", -10}, {"high", 30}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 4 && r.value["width"] == 2);
    const Terrain& t = f.state.scene.terrain;
    GIANT_CHECK(t.height_at(0, 0).value() == -10.0 && std::fabs(t.height_at(0, 100).value() - 30.0) < 1e-4);
    GIANT_CHECK(std::fabs(t.height_at(40, 50).value() - 10.0) < 1e-4); // halfway: bilinear
    GIANT_CHECK(t.find({1, 1})->at(16 * 16 - 1) == 30.0f); // past the area on a new tile: the image's edge
    GIANT_CHECK(f.run("undo").ok && t.size() == 1 && t.height_at(10, 10).value() == 7.0);
    GIANT_CHECK(!f.run("import_height_map", {{"file", "no/such/file.r16"}, {"from", {0, 0}}, {"to", {1, 1}}}).ok);
    std::filesystem::remove(file);
}

GIANT_TEST("terrain brush tool: a ray from the camera finds the ground under the pointer") {
    Terrain t;
    t.set_samples(16);
    t.set({0, 0}, Terrain::Heights(256, 3.0f));
    EditorCamera camera;
    camera.position = {32.0, 50.0, 32.0};
    camera.pitch_degrees = -89.0f;
    const Vec3 down = camera_ray(camera, 400.0f, 300.0f, 800.0f, 600.0f); // the middle of the window
    GIANT_CHECK(down.y < -0.99f);
    const auto hit = terrain_ray_hit(t, camera.position, down, 1000.0);
    GIANT_CHECK(hit.has_value() && std::fabs(hit->y - 3.0) < 1e-3 && std::fabs(hit->x - 32.0) < 1.0);
    // Looking up, or away from all ground: no hit.
    GIANT_CHECK(!terrain_ray_hit(t, camera.position, {0.0f, 1.0f, 0.0f}, 1000.0).has_value());
    GIANT_CHECK(!terrain_ray_hit(t, {500.0, 50.0, 500.0}, {0.0f, -1.0f, 0.0f}, 1000.0).has_value());
    // The left edge of the window looks left of the middle.
    camera.pitch_degrees = 0.0f;
    const Vec3 left = camera_ray(camera, 0.0f, 300.0f, 800.0f, 600.0f);
    GIANT_CHECK(left.x < -0.5f && std::fabs(left.y) < 1e-5f);
}

GIANT_TEST("terrain brush tool: a drag in the viewport sculpts under the pointer, as one undo step") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {63, 63}}, {"samples", 16}, {"height", 0}}).ok);
    f.state.camera.position = {32.0, 40.0, 32.0};
    f.state.camera.pitch_degrees = -89.0f;
    f.state.brush = {true, "raise", 6.0, 1.0f, 0.0f, 0.0f};
    TerrainBrushTool tool;
    const usize steps = f.commands.undo_history().size();
    platform::InputEvent press;
    press.type = platform::InputEvent::Type::mouse_button;
    press.button = platform::MouseButton::left;
    press.down = true;
    press.x = 400.0f;
    press.y = 300.0f;
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    const Terrain& t = f.state.scene.terrain;
    const double first = t.height_at(32.0, 32.0).value();
    GIANT_CHECK(first > 0.9 && tool.stroking());
    tool.update({}, false, 800.0f, 600.0f, 0.001f, f.state, f.commands); // held still, too soon: no dab
    GIANT_CHECK(t.height_at(32.0, 32.0).value() == first);
    tool.update({}, false, 800.0f, 600.0f, 0.05f, f.state, f.commands); // held: another dab
    GIANT_CHECK(t.height_at(32.0, 32.0).value() > first);
    platform::InputEvent release = press;
    release.down = false;
    tool.update({release}, false, 800.0f, 600.0f, 0.05f, f.state, f.commands);
    GIANT_CHECK(!tool.stroking() && f.commands.undo_history().size() == steps + 1);
    GIANT_CHECK(f.run("undo").ok && t.height_at(32.0, 32.0).value() == 0.0);
    // Over a panel, or with the tool off, a click does nothing.
    tool.update({press}, true, 800.0f, 600.0f, 0.05f, f.state, f.commands);
    GIANT_CHECK(!tool.stroking());
    f.state.brush.enabled = false;
    tool.update({press}, false, 800.0f, 600.0f, 0.05f, f.state, f.commands);
    GIANT_CHECK(!tool.stroking() && t.height_at(32.0, 32.0).value() == 0.0);
}

GIANT_TEST("terrain brush tool: a jump of the pointer starts the stroke again; flatten holds the start's height") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {255, 63}}, {"samples", 16}, {"height", 0}}).ok);
    f.state.camera.position = {32.0, 40.0, 32.0};
    f.state.camera.pitch_degrees = -89.0f;
    f.state.brush = {true, "raise", 4.0, 1.0f, 0.0f, 0.0f};
    TerrainBrushTool tool;
    platform::InputEvent press;
    press.type = platform::InputEvent::Type::mouse_button;
    press.button = platform::MouseButton::left;
    press.down = true;
    press.x = 400.0f;
    press.y = 300.0f;
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    // The camera flies 160 m with the button held: no ridge between.
    f.state.camera.position.x += 160.0;
    tool.update({}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    const Terrain& t = f.state.scene.terrain;
    GIANT_CHECK(t.height_at(32.0, 32.0).value() > 0.9 && t.height_at(192.0, 32.0).value() > 0.9);
    GIANT_CHECK(t.height_at(36.0, 32.0).value() == 0.0);
    GIANT_CHECK(t.height_at(112.0, 32.0).value() == 0.0);
    platform::InputEvent release = press;
    release.down = false;
    tool.update({release}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    // Flatten: to the ground where the stroke starts (1 m up), not to `height`
    // (the sample 4 m on was not raised).
    f.state.camera.position = {32.0, 40.0, 32.0};
    f.state.brush = {true, "flatten", 8.0, 1.0f, 0.0f, -50.0f};
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    const double start = t.height_at(32.0, 32.0).value();
    GIANT_CHECK(start > 0.9 && std::fabs(t.height_at(36.0, 32.0).value() - start) < 1e-3);
    tool.update({release}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    // The ray reaches far ground in a few thousand steps.
    const auto far_hit = terrain_ray_hit(t, {-15000.0, 100.0, 32.0}, normalize_or_zero(Vec3{1.0f, -0.00662f, 0.0f}),
                                         20000.0);
    GIANT_CHECK(far_hit.has_value() && far_hit->x > 0.0 && far_hit->x < 256.0);
}

GIANT_TEST("terrain brush tool: a fast drag over far ground leaves no gaps") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {511, 63}}, {"samples", 16}, {"height", 0}}).ok);
    // Low over the ground, looking along x at a grazing angle: a pixel is
    // metres of ground there.
    f.state.camera.position = {0.0, 10.0, 32.0};
    f.state.camera.yaw_degrees = -90.0f; // along +x
    f.state.camera.pitch_degrees = -2.0f;
    f.state.brush = {true, "raise", 4.0, 1.0f, 0.0f, 0.0f};
    const Terrain& t = f.state.scene.terrain;
    // Where the pointer starts and ends, on the ground before it is raised.
    const auto near_hit =
        terrain_ray_hit(t, f.state.camera.position,
                        camera_ray(f.state.camera, 400.0f, 368.0f, 800.0f, 600.0f), 1000.0);
    const auto far_hit =
        terrain_ray_hit(t, f.state.camera.position,
                        camera_ray(f.state.camera, 400.0f, 316.0f, 800.0f, 600.0f), 1000.0);
    TerrainBrushTool tool;
    platform::InputEvent press;
    press.type = platform::InputEvent::Type::mouse_button;
    press.button = platform::MouseButton::left;
    press.down = true;
    press.x = 400.0f;
    press.y = 368.0f; // ~60 m away
    const Vec3 forward = camera_ray(f.state.camera, 400.0f, 300.0f, 800.0f, 600.0f);
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    platform::InputEvent move = press;
    move.type = platform::InputEvent::Type::mouse_move;
    move.y = 316.0f; // 52 pixels up in a frame: ~150 m away
    tool.update({move}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    GIANT_CHECK(forward.x > 0.9f && near_hit && far_hit && far_hit->x - near_hit->x > 50.0);
    for (double x = near_hit->x; x <= far_hit->x; x += 2.0) {
        GIANT_CHECK(t.height_at(x, 32.0).value() > 0.5);
    }
}

GIANT_TEST("terrain brush tool: the pointer over a ridge starts the line again behind it") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {1023, 63}}, {"samples", 16}, {"height", 0}}).ok);
    // A wall 30 m high at x 100 to 108, seen from 40 m up.
    Terrain& t = f.state.scene.terrain;
    Terrain::Heights* wall = t.modify({1, 0});
    for (usize j = 0; j < 16; ++j) {
        for (usize i = 9; i <= 11; ++i) (*wall)[j * 16 + i] = 30.0f; // x = 100, 104, 108
    }
    f.state.camera.position = {0.0, 40.0, 32.0};
    f.state.camera.yaw_degrees = -90.0f;
    f.state.camera.pitch_degrees = -10.0f;
    f.state.brush = {true, "raise", 4.0, 1.0f, 0.0f, 0.0f};
    // The rows of the window just on the wall and just past it.
    const auto hit_x = [&](float y) {
        const auto h = terrain_ray_hit(t, f.state.camera.position, camera_ray(f.state.camera, 400.0f, y, 800.0f, 600.0f),
                                       5000.0);
        return h ? h->x : -1.0;
    };
    float on = 600.0f;
    while (on > 0.0f && hit_x(on - 1.0f) < 150.0) on -= 1.0f; // last row hitting the wall
    GIANT_CHECK(hit_x(on) < 110.0 && hit_x(on - 2.0f) > 200.0);
    // Where the rays land before anything is raised.
    const double behind = hit_x(on - 10.0f);
    const double front = hit_x(on + 20.0f);
    const double back = hit_x(on - 20.0f);
    TerrainBrushTool tool;
    platform::InputEvent press;
    press.type = platform::InputEvent::Type::mouse_button;
    press.button = platform::MouseButton::left;
    press.down = true;
    press.x = 400.0f;
    press.y = on;
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    platform::InputEvent move = press;
    move.type = platform::InputEvent::Type::mouse_move;
    move.y = on - 10.0f; // ten pixels up: over the top (raised 1 m by the first dab)
    tool.update({move}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    const auto raised_past = [&](double from) { // any ground raised from `from` on
        for (double x = from; x < 1000.0; x += 1.0) {
            if (t.height_at(x, 32.0).value() > 0.5) return true;
        }
        return false;
    };
    GIANT_CHECK(behind > 420.0 && raised_past(420.0)); // dabbed where it landed
    for (double x = 120.0; x < 400.0; x += 4.0) GIANT_CHECK(t.height_at(x, 32.0).value() == 0.0);
    platform::InputEvent release = move;
    release.type = platform::InputEvent::Type::mouse_button;
    release.down = false;
    tool.update({release}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    GIANT_CHECK(f.run("undo").ok && !raised_past(120.0) && t.height_at(104.0, 32.0).value() == 30.0);
    // A fast drag over it (40 pixels a frame): the same.
    press.y = on + 20.0f;
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    move.y = on - 20.0f;
    tool.update({move}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    GIANT_CHECK(front < 100.0 && back > 420.0 && t.height_at(front, 32.0).value() > 0.5 && raised_past(420.0));
    for (double x = 120.0; x < 400.0; x += 4.0) GIANT_CHECK(t.height_at(x, 32.0).value() == 0.0);
}

GIANT_TEST("terrain brush tool: a fast drag over a low crest at a grazing angle does not bridge the valley") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {2047, 63}}, {"samples", 16}, {"height", 0}}).ok);
    // A crest 9 m high 48 m out, seen from 10 m up: the valley behind it is hidden.
    Terrain& t = f.state.scene.terrain;
    Terrain::Heights* crest = t.modify({0, 0});
    for (usize j = 0; j < 16; ++j) {
        (*crest)[j * 16 + 11] = 4.5f; // x = 44
        (*crest)[j * 16 + 12] = 9.0f; // x = 48
        (*crest)[j * 16 + 13] = 4.5f; // x = 52
    }
    f.state.camera.position = {0.0, 10.0, 32.0};
    f.state.camera.yaw_degrees = -90.0f;
    f.state.camera.pitch_degrees = -2.0f;
    f.state.brush = {true, "lower", 4.0, 1.0f, 0.0f, 0.0f}; // (raising would hide the valley more)
    const auto hit_x = [&](float y) {
        const auto h = terrain_ray_hit(t, f.state.camera.position,
                                       camera_ray(f.state.camera, 400.0f, y, 800.0f, 600.0f), 5000.0);
        return h ? h->x : -1.0;
    };
    float on = 600.0f;
    while (on > 0.0f && hit_x(on - 1.0f) < 49.0) on -= 1.0f; // last row hitting the crest's front
    for (const float speed : {6.0f, 20.0f, 40.0f}) { // pixels a frame, ending just over the crest
        const double from = hit_x(on + speed - 2.0f);
        const double to = hit_x(on - 2.0f);
        GIANT_CHECK(from > 0.0 && from < 49.0 && to > 150.0);
        TerrainBrushTool tool;
        platform::InputEvent press;
        press.type = platform::InputEvent::Type::mouse_button;
        press.button = platform::MouseButton::left;
        press.down = true;
        press.x = 400.0f;
        press.y = on + speed - 2.0f;
        tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
        platform::InputEvent move = press;
        move.type = platform::InputEvent::Type::mouse_move;
        move.y = on - 2.0f;
        tool.update({move}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
        platform::InputEvent release = move;
        release.type = platform::InputEvent::Type::mouse_button;
        release.down = false;
        tool.update({release}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
        for (double x = 64.0; x < std::min(to - 10.0, 140.0); x += 4.0) GIANT_CHECK(t.height_at(x, 32.0).value() == 0.0);
        GIANT_CHECK(f.run("undo").ok);
    }
}

GIANT_TEST("terrain commands: paint a layer and erase it, read back, undone in one step a drag; sculpting keeps it") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {127, 63}}, {"samples", 16}, {"height", 2}}).ok);
    const Terrain& t = f.state.scene.terrain;
    const auto weight = [&f](double x, double z, int layer) {
        const ai::CommandResult r = f.run("get_terrain", {{"points", {{x, z}}}});
        GIANT_CHECK(r.ok);
        return r.value["paint"][0][static_cast<usize>(layer - 1)].get<double>();
    };
    ai::CommandResult r = f.run("paint_terrain", {{"layer", 2}, {"points", {{20, 30}, {100, 30}}}, {"radius", 6},
                                                  {"strength", 1}, {"falloff", 0}});
    GIANT_CHECK(r.ok && r.value["tiles_changed"] == 2);
    GIANT_CHECK(r.value.contains("warning")); // no layers set yet: it says so, and paints all the same
    GIANT_CHECK(weight(60, 30, 2) == 1.0 && weight(60, 30, 1) == 0.0 && weight(60, 50, 2) == 0.0);
    GIANT_CHECK(t.height_at(60, 30).value() == 2.0); // heights untouched
    r = f.run("get_terrain");
    GIANT_CHECK(r.ok && r.value["painted_tiles"] == 2);
    GIANT_CHECK(f.run("get_terrain", {{"points", {{5000, 5000}}}}).value["paint"][0].is_null()); // no tile
    GIANT_CHECK(f.run("undo").ok && weight(60, 30, 2) == 0.0 && t.paints().empty());
    GIANT_CHECK(f.run("redo").ok && weight(60, 30, 2) == 1.0);

    // A sculpt over painted ground, undone and redone, keeps the paint.
    GIANT_CHECK(f.run("sculpt_terrain", {{"op", "raise"}, {"points", {{60, 30}}}, {"radius", 6}, {"strength", 1}}).ok);
    GIANT_CHECK(t.height_at(60, 30).value() > 2.0 && weight(60, 30, 2) == 1.0);
    GIANT_CHECK(f.run("undo").ok && t.height_at(60, 30).value() == 2.0 && weight(60, 30, 2) == 1.0);
    GIANT_CHECK(f.run("redo").ok && weight(60, 30, 2) == 1.0);
    GIANT_CHECK(f.run("undo").ok);

    // Erase, as a drag of two calls: one undo step; erased to nothing, no paint is kept.
    const usize steps = f.commands.undo_history().size();
    Json erase = {{"op", "erase"}, {"layer", 2}, {"points", {{20, 30}, {60, 30}}}, {"radius", 6}, {"strength", 1},
                  {"falloff", 0}};
    GIANT_CHECK(f.commands.execute("paint_terrain", erase, {88, true}).ok);
    erase["points"] = {{60, 30}, {100, 30}};
    erase["continues"] = true;
    GIANT_CHECK(f.commands.execute("paint_terrain", erase, {88, true}).ok);
    GIANT_CHECK(f.commands.undo_history().size() == steps + 1 && t.paints().empty());
    GIANT_CHECK(f.run("undo").ok && weight(20, 30, 2) == 1.0 && weight(100, 30, 2) == 1.0);
    // Half strength goes half way; layer 7 is the last; bad layers and ops are refused.
    GIANT_CHECK(f.run("paint_terrain", {{"layer", 7}, {"points", {{60, 10}}}, {"strength", 0.5}, {"falloff", 0}}).ok);
    GIANT_CHECK(std::fabs(weight(60, 10, 7) - 128.0 / 255.0) < 1e-9);
    GIANT_CHECK(!f.run("paint_terrain", {{"layer", 0}, {"points", {{60, 10}}}}).ok);
    GIANT_CHECK(!f.run("paint_terrain", {{"layer", 8}, {"points", {{60, 10}}}}).ok);
    GIANT_CHECK(!f.run("paint_terrain", {{"layer", 2}, {"op", "raise"}, {"points", {{60, 10}}}}).ok);
    GIANT_CHECK(!f.run("paint_terrain", {{"layer", 2}, {"points", {{60, 10}}}, {"strength", 2}}).ok);
    // Nothing there: nothing changes, no undo step.
    const usize now = f.commands.undo_history().size();
    r = f.run("paint_terrain", {{"layer", 2}, {"points", {{5000, 5000}}}});
    GIANT_CHECK(r.ok && r.value["tiles_changed"] == 0 && f.commands.undo_history().size() == now);
    // With a layer there, no warning.
    GIANT_CHECK(f.run("set_terrain_layers", {{"layers", {Json::object(), Json::object(), Json::object()}}}).ok);
    r = f.run("paint_terrain", {{"layer", 2}, {"points", {{60, 50}}}});
    GIANT_CHECK(r.ok && !r.value.contains("warning"));
}

GIANT_TEST("terrain brush tool: paint mode paints the chosen layer under the pointer, as one undo step") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {63, 63}}, {"samples", 16}, {"height", 0}}).ok);
    f.state.camera.position = {32.0, 40.0, 32.0};
    f.state.camera.pitch_degrees = -89.0f;
    f.state.brush = {true, "paint", 6.0, 1.0f, 0.0f, 0.0f};
    f.state.brush.layer = 3;
    TerrainBrushTool tool;
    const usize steps = f.commands.undo_history().size();
    platform::InputEvent press;
    press.type = platform::InputEvent::Type::mouse_button;
    press.button = platform::MouseButton::left;
    press.down = true;
    press.x = 400.0f;
    press.y = 300.0f;
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    const Terrain& t = f.state.scene.terrain;
    const Terrain::Paint* p = t.paint({0, 0});
    const usize centre = (8 * 16 + 8) * world::k_paint_channels; // the sample at (32, 32)
    GIANT_CHECK(p != nullptr && (*p)[centre + 2] == 255 && (*p)[centre + 1] == 0);
    GIANT_CHECK(t.height_at(32.0, 32.0).value() == 0.0);
    platform::InputEvent release = press;
    release.down = false;
    tool.update({release}, false, 800.0f, 600.0f, 0.05f, f.state, f.commands);
    GIANT_CHECK(f.commands.undo_history().size() == steps + 1);
    // Erase takes it off.
    f.state.brush.op = "erase";
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    tool.update({release}, false, 800.0f, 600.0f, 0.05f, f.state, f.commands);
    GIANT_CHECK(t.paints().empty() && f.commands.undo_history().size() == steps + 2);
    GIANT_CHECK(f.run("undo").ok && t.paint({0, 0}) != nullptr);
    GIANT_CHECK(f.run("undo").ok && t.paints().empty());
}

GIANT_TEST("terrain brush tool: paint after a strong raise sends strength 1; get_terrain reads paint at tile edges") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {63, 63}}, {"samples", 16}, {"height", 0}}).ok);
    f.state.camera.position = {32.0, 40.0, 32.0};
    f.state.camera.pitch_degrees = -89.0f;
    f.state.brush = {true, "paint", 6.0, 5.0f, 0.0f, 0.0f}; // 5 m from a raise
    TerrainBrushTool tool;
    platform::InputEvent press;
    press.type = platform::InputEvent::Type::mouse_button;
    press.button = platform::MouseButton::left;
    press.down = true;
    press.x = 400.0f;
    press.y = 300.0f;
    tool.update({press}, false, 800.0f, 600.0f, 0.016f, f.state, f.commands);
    GIANT_CHECK(tool.stroking() && f.state.scene.terrain.paint({0, 0}) != nullptr);
    // Past the last sample of a tile with no tile beyond: its own last sample, not null.
    GIANT_CHECK(f.run("paint_terrain", {{"layer", 1}, {"points", {{63, 63}}}, {"radius", 6}, {"falloff", 0},
                                        {"strength", 1}}).ok);
    const ai::CommandResult r = f.run("get_terrain", {{"points", {{63.9, 63.9}, {64.5, 10}}}});
    GIANT_CHECK(r.ok && r.value["heights"][0].is_number() && r.value["paint"][0][0] == 1.0);
    GIANT_CHECK(r.value["paint"][1].is_null() && r.value["heights"][1].is_null());
    // On a tile's far edge, with only it there: its last sample.
    GIANT_CHECK(f.run("get_terrain", {{"points", {{64, 63}}}}).value["paint"][0][0] == 1.0);
    // With a tile past it, the nearest sample is that tile's first, as the brush paints it.
    GIANT_CHECK(f.run("create_terrain", {{"from", {64, 0}}, {"to", {127, 63}}, {"samples", 16}, {"height", 0}}).ok);
    GIANT_CHECK(f.run("paint_terrain", {{"layer", 2}, {"points", {{64, 8}}}, {"radius", 1}, {"falloff", 0},
                                        {"strength", 1}}).ok);
    const ai::CommandResult both = f.run("get_terrain", {{"points", {{63.9, 8}, {61.9, 8}, {64.1, 8}}}});
    GIANT_CHECK(both.value["paint"][0][1] == 1.0 && both.value["paint"][1][1] == 0.0 &&
                both.value["paint"][2][1] == 1.0);
}
