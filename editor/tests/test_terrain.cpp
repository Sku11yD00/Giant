// Terrain in the editor: tile meshes, drawing by cell, and the commands.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"
#include "../src/terrain_meshes.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
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
    // A new look: only the cells with ground.
    scene.terrain.material.base_color = {0.5f, 0.1f, 0.1f};
    build();
    GIANT_CHECK(frame.cells_rebuilt == 2);
    for (const render::Instance& i : live()) {
        if (i.always_mesh) GIANT_CHECK(frame.materials[i.material].base_color == scene.terrain.material.base_color);
    }
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
