// Scatter in the editor: layers placed over the ground on a worker, drawn
// with their cells, changed by undoable commands.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scatter_placer.h"
#include "../src/scene_render.h"
#include "../src/terrain_meshes.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <algorithm>
#include <chrono>
#include <thread>

using namespace giant;
using namespace giant::editor;
using ai::Json;
using world::CellCoord;
using world::Terrain;

namespace {

void flat(Terrain& t, i32 x0, i32 z0, i32 x1, i32 z1, float height = 0.0f) {
    t.set_samples(16);
    for (i32 z = z0; z <= z1; ++z) {
        for (i32 x = x0; x <= x1; ++x) t.set({x, z}, Terrain::Heights(256, height));
    }
}

world::ScatterLayer stones(float spacing = 8.0f) {
    world::ScatterLayer l;
    l.name = "stones";
    l.spacing = spacing;
    world::ScatterItem i;
    i.shape = world::Shape::sphere;
    l.items = {i};
    return l;
}

// Places what the scene needs, as a frame would after the terrain's changes.
void place(ScatterPlacer& p, world::Scene& scene) { p.finish(scene, scene.terrain.take_changes()); }

u64 points_in(const ScatterPlacer& p, CellCoord c) {
    const ScatterPlacer::Cell* cell = p.find(c);
    return cell == nullptr ? 0 : cell->points.size();
}

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    TerrainMeshes terrain{nullptr};
    ScatterPlacer scatter;
    WorldStreaming world;
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        state.terrain_meshes = &terrain;
        state.scatter = &scatter;
        state.world = &world;
        register_scene_commands(commands, state);
        register_terrain_commands(commands, state);
        register_scatter_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};

} // namespace

GIANT_TEST("scatter placer: cells with ground are placed as the rules say, again only where things changed") {
    world::Scene scene;
    flat(scene.terrain, 0, 0, 3, 3);
    scene.scatter.push_back(stones());
    ScatterPlacer p;
    place(p, scene);
    const ScatterStats s = p.stats();
    GIANT_CHECK(s.cells == 16 && s.points == 16 * 64 && s.building == 0 && s.placed == 16);
    std::vector<world::ScatterPoint> expected;
    world::scatter_cell(scene.scatter[0], 0, scene.terrain, {2, 1}, expected);
    const ScatterPlacer::Cell* cell = p.find({2, 1});
    GIANT_CHECK(cell != nullptr && cell->points.size() == expected.size());
    GIANT_CHECK(cell->points[5].transform == expected[5].transform);
    GIANT_CHECK(p.take_changed().size() == 16);
    // A tile changed: it and its neighbours are placed again (the others not).
    scene.terrain.modify({0, 0})->at(0) = 2.0f;
    place(p, scene);
    GIANT_CHECK(p.stats().placed == 16 + 4);
    std::vector<CellCoord> changed = p.take_changed();
    std::sort(changed.begin(), changed.end(), [](CellCoord a, CellCoord b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
    GIANT_CHECK(changed.size() == 4 && changed[0] == (CellCoord{0, 0}) && changed[3] == (CellCoord{1, 1}));
    // A tile gone: its cell's points go.
    scene.terrain.remove({3, 3});
    place(p, scene);
    GIANT_CHECK(p.find({3, 3}) == nullptr && p.stats().cells == 15);
    // The layers changed: every cell again; none left: nothing placed.
    scene.scatter[0].density = 0.5f;
    place(p, scene);
    GIANT_CHECK(p.stats().points < 15 * 64 && p.stats().points > 15 * 16);
    scene.scatter[0].visible = false;
    place(p, scene);
    GIANT_CHECK(p.stats().cells == 0 && p.stats().points == 0);
}

GIANT_TEST("scatter placer: a frame swaps in a budget of instances, a whole cell at a time") {
    world::Scene scene;
    flat(scene.terrain, 0, 0, 3, 0);
    scene.scatter.push_back(stones(1.0f)); // 4096 points a cell
    ScatterPlacer p;
    p.instances_per_frame = 5000;
    p.cells_in_flight = 4;
    world::TerrainChanges changes = scene.terrain.take_changes();
    p.update(scene, changes);
    usize frames = 0;
    std::vector<u32> cells_seen;
    while (p.stats().cells < 4 && frames < 20000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // the worker's time
        p.update(scene, {});
        cells_seen.push_back(p.stats().cells);
        for (const CellCoord c : p.take_changed()) GIANT_CHECK(points_in(p, c) == 4096); // whole cells only
        ++frames;
    }
    GIANT_CHECK(p.stats().cells == 4 && p.stats().points == 4 * 4096);
    // At most one cell a frame came in (5000 instances a frame is one cell of 4096).
    for (usize k = 1; k < cells_seen.size(); ++k) GIANT_CHECK(cells_seen[k] - cells_seen[k - 1] <= 1);
}

GIANT_TEST("scatter placer: near the camera only, nearest first, within a budget; a new scene drops the old at once") {
    world::Scene scene;
    flat(scene.terrain, 0, 0, 9, 0); // ten cells in a row
    scene.scatter.push_back(stones(4.0f)); // 256 points a cell
    ScatterPlacer p;
    p.camera = {32.0, 0.0, 32.0};
    p.radius = 200.0; // cells 0 to 3 (centres 32 to 224 m)
    place(p, scene);
    GIANT_CHECK(p.stats().cells == 4 && p.find({3, 0}) != nullptr && p.find({4, 0}) == nullptr);
    // The camera moves: the cells now near come, the far go.
    p.camera = {600.0, 0.0, 32.0};
    place(p, scene);
    GIANT_CHECK(p.find({0, 0}) == nullptr && p.find({9, 0}) != nullptr && p.find({6, 0}) != nullptr);
    // Nearest first: with a budget of 3 cells and room for more, the nearest fill it.
    p.max_instances = 3 * 256 + 100;
    p.radius = 10000.0;
    p.camera = {32.0, 0.0, 32.0};
    ScatterPlacer q;
    q.camera = p.camera;
    q.radius = p.radius;
    q.max_instances = p.max_instances;
    q.cells_in_flight = 1; // one at a time, in the queue's order
    q.update(scene, scene.terrain.take_changes());
    for (int frames = 0; frames < 20000 && q.stats().building > 0; ++frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        q.update(scene, {});
    }
    GIANT_CHECK(q.instances() <= q.max_instances);
    GIANT_CHECK(points_in(q, {0, 0}) == 256 && points_in(q, {1, 0}) == 256 && points_in(q, {2, 0}) == 256);
    GIANT_CHECK(points_in(q, {3, 0}) == 100 && q.find({4, 0}) == nullptr && q.stats().cut_short >= 1);
    // Full, the camera moves: the cells left behind make room for the new.
    q.camera = {600.0, 0.0, 32.0};
    q.radius = 200.0; // cells 6 to 9
    q.update(scene, {});
    for (int frames = 0; frames < 20000 && q.stats().building > 0; ++frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        q.update(scene, {});
    }
    GIANT_CHECK(q.find({0, 0}) == nullptr && q.instances() == q.max_instances);
    GIANT_CHECK(points_in(q, {7, 0}) == 256 && points_in(q, {8, 0}) == 256 && q.stats().building == 0);
    // Settled with cells left out: nothing is placed again and again.
    const u64 settled = q.stats().placed;
    for (int frames = 0; frames < 50; ++frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        q.update(scene, {});
    }
    GIANT_CHECK(q.stats().placed == settled);
    // All at once (more in flight than one, any order): nearer cells win room from farther ones.
    ScatterPlacer n;
    n.camera = {32.0, 0.0, 32.0};
    n.radius = 10000.0;
    n.max_instances = 3 * 256 + 100;
    place(n, scene);
    GIANT_CHECK(n.instances() <= n.max_instances);
    GIANT_CHECK(points_in(n, {0, 0}) == 256 && points_in(n, {1, 0}) == 256 && points_in(n, {2, 0}) == 256);
    // A kit item counts its pieces.
    world::Kit kit;
    kit.name = "pair";
    kit.pieces.resize(2);
    scene.kits.push_back(kit);
    world::ScatterItem item;
    item.shape = world::Shape::kit;
    item.kit = "pair";
    scene.scatter[0].items = {item};
    ScatterPlacer k;
    k.camera = {32.0, 0.0, 32.0};
    k.radius = 40.0; // cell 0 only
    place(k, scene);
    GIANT_CHECK(points_in(k, {0, 0}) == 256 && k.instances() == 512);
    // The kit grows: the cell is placed again and counts the new pieces.
    scene.kits[0].pieces.resize(3);
    place(k, scene);
    GIANT_CHECK(points_in(k, {0, 0}) == 256 && k.instances() == 768);
    // Another scene (another terrain): the old placements go in the same frame.
    world::Scene other;
    flat(other.terrain, 0, 0, 0, 0);
    other.scatter = scene.scatter;
    other.kits = scene.kits;
    k.update(other, other.terrain.take_changes());
    GIANT_CHECK(k.find({0, 0}) == nullptr && k.instances() == 0);
    place(k, other);
    GIANT_CHECK(points_in(k, {0, 0}) == 256);
}

GIANT_TEST("scatter: placements draw with their cell, as their items, and follow the placer") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    if (renderer == nullptr) return;
    MeshLibrary meshes(renderer.get());
    world::Scene scene;
    flat(scene.terrain, 0, 0, 1, 0, 3.0f);
    world::ScatterLayer l = stones();
    l.items[0].material.base_color = {0.1f, 0.2f, 0.3f};
    // A kit item too: each placement draws its two pieces.
    world::Kit tree;
    tree.name = "tree";
    tree.pieces.resize(2);
    tree.pieces[0].shape = world::Shape::cube;
    tree.pieces[1].shape = world::Shape::sphere;
    tree.pieces[1].transform.position = {0.0, 3.0, 0.0};
    scene.kits.push_back(tree);
    world::ScatterItem kit;
    kit.shape = world::Shape::kit;
    kit.kit = "tree";
    l.items.push_back(kit);
    scene.scatter.push_back(l);
    ScatterPlacer p;
    place(p, scene);
    SceneFrame frame;
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    usize spheres = 0, kits = 0;
    for (const CellCoord c : p.cells()) {
        for (const world::ScatterPoint& pt : p.find(c)->points) (pt.item == 0 ? spheres : kits) += 1;
    }
    usize live = 0;
    for (const render::Instance& i : frame.instances) live += i.mesh != render::k_no_mesh ? 1 : 0;
    GIANT_CHECK(spheres > 0 && kits > 0 && live == spheres + 2 * kits);
    GIANT_CHECK(frame.cells.size() == 2);
    // One material for the stones, two for the kit's pieces (both default grey: one).
    GIANT_CHECK(frame.materials.size() == 2);
    // Nothing changed: nothing rebuilt; the layer changed: the cells again.
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    GIANT_CHECK(frame.cells_rebuilt == 0);
    scene.scatter[0].items.pop_back();
    place(p, scene);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    live = 0;
    for (const render::Instance& i : frame.instances) live += i.mesh != render::k_no_mesh ? 1 : 0;
    GIANT_CHECK(frame.cells_rebuilt == 2 && live == p.stats().points);
    // A kit changed: scattered cells rebuild.
    scene.scatter[0].items.push_back(kit);
    place(p, scene);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    scene.kits[0].pieces.pop_back();
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    GIANT_CHECK(frame.cells_rebuilt == 2);
    // The kit grows (the placer not yet run again): the cells draw no more
    // than the placer counted.
    const u64 counted = p.instances();
    scene.kits[0].pieces.resize(40);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    live = 0;
    for (const render::Instance& i : frame.instances) live += i.mesh != render::k_no_mesh ? 1 : 0;
    GIANT_CHECK(live <= counted + 2 * 40); // a point's pieces past it, a cell
    place(p, scene); // placed again, counting 40 pieces
    GIANT_CHECK(p.instances() > counted);
    scene.kits[0].pieces.resize(1);
    // Everything gone: the cells go.
    scene.scatter.clear();
    place(p, scene);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    live = 0;
    for (const render::Instance& i : frame.instances) live += i.mesh != render::k_no_mesh ? 1 : 0;
    GIANT_CHECK(live == 0);
}

GIANT_TEST("scatter commands: add, change, list and remove layers, all undoable") {
    Fixture f;
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {127, 63}}, {"samples", 16}, {"height", 1}}).ok);
    ai::CommandResult r = f.run("add_scatter_layer", {{"name", "rocks"},
                                                     {"items", {{{"shape", "sphere"}, {"scale", {0.5, 2}}},
                                                                {{"shape", "cube"}, {"weight", 2}, {"align", 1}}}},
                                                     {"spacing", 4},
                                                     {"slope_max", 30},
                                                     {"area", {{"from", {100, 50}}, {"to", {0, 0}}}}});
    GIANT_CHECK(r.ok && r.value["area"]["from"][0] == 0.0 && r.value["area"]["to"][0] == 100.0);
    GIANT_CHECK(f.state.scene.scatter.size() == 1 && f.state.scene.scatter[0].items[1].weight == 2.0f);
    f.scatter.finish(f.state.scene, f.state.scene.terrain.take_changes());
    r = f.run("get_scatter");
    GIANT_CHECK(r.ok && r.value["layers"].size() == 1 && r.value["building"] == 0 && r.value["cells"] == 2);
    const u64 points = r.value["points"].get<u64>();
    GIANT_CHECK(points > 200 && r.value["layers"][0]["points"] == points);
    // Change: only what is given.
    r = f.run("set_scatter_layer", {{"name", "rocks"}, {"density", 0.5}, {"area", nullptr}, {"new_name", "stones"}});
    GIANT_CHECK(r.ok && r.value["name"] == "stones" && r.value["area"].is_null() && r.value["spacing"] == 4.0);
    GIANT_CHECK(f.state.scene.scatter[0].items.size() == 2 && !f.state.scene.scatter[0].area);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.scatter[0].name == "rocks" && f.state.scene.scatter[0].area);
    GIANT_CHECK(f.run("redo").ok && f.state.scene.scatter[0].name == "stones");
    // Errors change nothing.
    GIANT_CHECK(!f.run("add_scatter_layer", {{"name", "stones"}, {"items", {{{"shape", "cube"}}}}}).ok);
    GIANT_CHECK(!f.run("add_scatter_layer", {{"name", "trees"}, {"items", {{{"kit", "oak"}}}}}).ok);
    GIANT_CHECK(!f.run("add_scatter_layer", {{"name", "x"}, {"items", {{{"shape", "decal"}}}}}).ok);
    GIANT_CHECK(!f.run("add_scatter_layer", {{"name", "x"}, {"items", Json::array()}}).ok);
    GIANT_CHECK(!f.run("set_scatter_layer", {{"name", "stones"}, {"slope_min", 60}, {"slope_max", 10}}).ok);
    GIANT_CHECK(!f.run("set_scatter_layer", {{"name", "stones"}, {"area", "everywhere"}}).ok);
    GIANT_CHECK(!f.run("set_scatter_layer", {{"name", "nope"}, {"density", 1}}).ok);
    GIANT_CHECK(!f.run("add_scatter_layer", {{"name", "x"}, {"items", {{{"mesh", "no/such/file.glb"}}}}}).ok);
    GIANT_CHECK(f.state.scene.scatter.size() == 1 && f.state.scene.scatter[0].density == 0.5f);
    // A kit item.
    GIANT_CHECK(f.run("create_kit", {{"name", "oak"}, {"pieces", {{{"shape", "cube"}}}}}).ok);
    GIANT_CHECK(f.run("add_scatter_layer", {{"name", "trees"}, {"items", {{{"kit", "oak"}}}}, {"spacing", 16}}).ok);
    GIANT_CHECK(f.state.scene.scatter[1].items[0].shape == world::Shape::kit);
    GIANT_CHECK(!f.run("delete_kit", {{"name", "oak"}}).ok); // the layer places it
    GIANT_CHECK(f.state.scene.find_kit("oak") != nullptr);
    // A shape that disagrees with its kit or mesh, and clumps too small.
    GIANT_CHECK(!f.run("add_scatter_layer", {{"name", "x"}, {"items", {{{"kit", "oak"}, {"shape", "cube"}}}}}).ok);
    GIANT_CHECK(!f.run("set_scatter_layer", {{"name", "trees"}, {"clump_size", 1e-30}, {"clumping", 1}}).ok);
    GIANT_CHECK(!f.run("set_scatter_layer", {{"name", "trees"}, {"clump_size", 0.5}}).ok);
    GIANT_CHECK(f.run("set_scatter_layer", {{"name", "trees"}, {"clump_size", 1}}).ok);
    GIANT_CHECK(f.run("undo").ok);
    // Saved with the scene.
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value());
    GIANT_CHECK(loaded.scatter == f.state.scene.scatter);
    // Removed, and back.
    GIANT_CHECK(f.run("remove_scatter_layer", {{"name", "stones"}}).ok && f.state.scene.scatter.size() == 1);
    GIANT_CHECK(!f.run("remove_scatter_layer", {{"name", "stones"}}).ok);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.scatter.size() == 2 && f.state.scene.scatter[0].name == "stones");
}
