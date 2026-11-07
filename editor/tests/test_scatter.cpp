// Scatter in the editor: layers placed over the ground on a worker, drawn
// with their cells, changed by undoable commands.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/physics_state.h"
#include "../src/scatter_placer.h"
#include "../src/scene_render.h"
#include "../src/terrain_meshes.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

GIANT_TEST("scatter trees: trunks stand near the camera, an impact fells one, drawn where it lies; stopping stands it up") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    if (renderer == nullptr) return;
    MeshLibrary meshes(renderer.get());
    world::Scene scene;
    flat(scene.terrain, 0, 0, 1, 0, 2.0f);
    world::ScatterLayer l = stones(16.0f);
    l.name = "trees";
    l.jitter = 0.0f;
    l.items[0].tree = world::ScatterTree{};
    world::ScatterItem rock; // not a tree: never a trunk
    rock.weight = 1.0f;
    l.items.push_back(rock);
    scene.scatter.push_back(l);
    ScatterPlacer p;
    place(p, scene);
    PhysicsState physics;
    const DVec3 camera{64.0, 10.0, 32.0};
    physics.feed_forest(scene, p, camera);
    physics.flush(scene);
    usize trees = 0;
    CellCoord cell{};
    u32 point = 0;
    world::Transform foot;
    for (const CellCoord c : p.cells()) {
        const ScatterPlacer::Cell* placed = p.find(c);
        GIANT_CHECK(physics.forest().serial(c) == placed->serial);
        for (u32 k = 0; k < placed->points.size(); ++k) {
            if (placed->points[k].item != 0) continue;
            const DVec3 at = placed->points[k].transform.position;
            // The westmost (and then northmost): it falls east onto the ground.
            if (trees++ == 0 || at.x < foot.position.x || (at.x == foot.position.x && at.z < foot.position.z)) {
                cell = c;
                point = k;
                foot = placed->points[k].transform;
            }
        }
    }
    GIANT_CHECK(trees > 4);
    GIANT_CHECK(physics.forest().stats().trunks == trees && physics.forest().stats().waiting == 0);
    // A ray at a trunk names its tree.
    GIANT_CHECK(physics.forest().tree_of(physics.forest().body_of(cell, point)).has_value());
    // Fed again unchanged: nothing made again.
    const sim::BodyId body = physics.forest().body_of(cell, point);
    physics.feed_forest(scene, p, camera);
    GIANT_CHECK(physics.forest().body_of(cell, point) == body);
    SceneFrame frame;
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p);
    const auto drawn_at = [&](const DVec3& at) {
        for (const render::Instance& i : frame.instances) {
            if (i.mesh != render::k_no_mesh && i.position == at) return true;
        }
        return false;
    };
    GIANT_CHECK(drawn_at(foot.position));
    physics.set_simulating(true);
    const DVec3 blow = foot.position + DVec3{-1.0, 1.0, 0.0};
    const PhysicsState::Impact hit = physics.impact(blow, 1.5f, 1e5f);
    GIANT_CHECK(hit.felled == 1 && hit.loosened == 0);
    // Falling: the first frame draws it fallen (the cell rebuilt), the next
    // ones only move it (its instance made again in place, the renderer
    // told only it changed).
    physics.run_steps(10);
    std::vector<CellCoord> moving = physics.take_tree_redraw();
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees(),
                moving);
    GIANT_CHECK(frame.cells_rebuilt == 1);
    physics.run_steps(10);
    moving = physics.take_tree_redraw();
    GIANT_CHECK(moving.size() == 1 && moving[0] == cell);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees(),
                moving);
    GIANT_CHECK(frame.cells_rebuilt == 1);
    usize in_place = 0;
    for (const render::CellRange& c : frame.cells) {
        if (c.prev_version != 0 && c.changed_count == 1) ++in_place;
    }
    GIANT_CHECK(in_place == 1);
    GIANT_CHECK(drawn_at(physics.fallen_trees().at(cell).trees[0].at.position));
    physics.run_steps(220);
    const auto f = physics.fallen_trees().find(cell);
    GIANT_CHECK(f != physics.fallen_trees().end() && f->second.trees.size() == 1);
    if (f == physics.fallen_trees().end() || f->second.trees.empty()) return;
    const world::Transform lying = f->second.trees[0].at;
    GIANT_CHECK(f->second.trees[0].point == point && rotate(lying.rotation, Vec3{0.0f, 1.0f, 0.0f}).x > 0.8f);
    std::vector<CellCoord> redraw = physics.take_tree_redraw();
    GIANT_CHECK(redraw.size() == 1 && redraw[0] == cell);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees(),
                redraw);
    GIANT_CHECK(frame.cells_rebuilt == 1 && drawn_at(lying.position) && !drawn_at(foot.position));
    // Stopping: standing again, drawn where it stood, its trunk back. Past
    // the frame's share of whole-cell rebuilds, a cell waits for the next.
    physics.set_simulating(false);
    GIANT_CHECK(physics.fallen_trees().empty());
    redraw = physics.take_tree_redraw();
    frame.fallen_rebuilds_per_frame = 0;
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees(),
                redraw);
    GIANT_CHECK(frame.cells_rebuilt == 0 && drawn_at(lying.position));
    frame.fallen_rebuilds_per_frame = 8;
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees());
    GIANT_CHECK(frame.cells_rebuilt == 1 && drawn_at(foot.position));
    physics.flush(scene);
    GIANT_CHECK(physics.forest().stats().trunks == trees && physics.forest().stats().fallen == 0);
    // The layer's trees gone: placed again with none, no trunks.
    scene.scatter[0].items[0].tree.reset();
    place(p, scene);
    physics.feed_forest(scene, p, camera);
    physics.flush(scene);
    GIANT_CHECK(physics.forest().stats().trunks == 0);
    // A new scene: the forest starts afresh.
    scene.scatter[0].items[0].tree = world::ScatterTree{};
    place(p, scene);
    physics.feed_forest(scene, p, camera);
    physics.flush(scene);
    GIANT_CHECK(physics.forest().stats().trunks == trees);
    physics.set_simulating(true);
    GIANT_CHECK(physics.impact(blow, 1.5f, 1e5f).felled == 1);
    world::Scene other;
    ScatterPlacer none; // the new scene's: nothing placed yet
    physics.feed_forest(other, none, camera);
    GIANT_CHECK(physics.forest().cells().empty() && physics.forest().stats().trunks == 0);
    GIANT_CHECK(physics.fallen_trees().empty() && physics.forest().bodies() == 0);
    GIANT_CHECK(physics.world().size() == 0); // cleared whole: the old scene's ground and trunks gone
    physics.set_simulating(false);
}

GIANT_TEST("scatter trees: the commands report them, cells come a share a frame, a kit tree falls whole") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    if (renderer == nullptr) return;
    MeshLibrary meshes(renderer.get());
    PhysicsState physics;
    EditorState state;
    state.meshes = &meshes;
    state.physics = &physics;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_physics_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); };
    world::Scene& scene = state.scene;
    flat(scene.terrain, 0, 0, 3, 0, 0.0f);
    world::Kit pine;
    pine.name = "pine";
    pine.pieces.resize(2);
    pine.pieces[0].shape = world::Shape::cube;
    pine.pieces[0].transform.position = {0.0, 1.0, 0.0};
    pine.pieces[0].transform.scale = {0.4f, 2.0f, 0.4f};
    pine.pieces[1].shape = world::Shape::sphere;
    pine.pieces[1].transform.position = {0.0, 6.0, 0.0};
    scene.kits.push_back(pine);
    world::ScatterLayer l = stones(16.0f);
    l.name = "pines";
    l.jitter = 0.0f;
    l.items[0].shape = world::Shape::kit;
    l.items[0].kit = "pine";
    l.items[0].tree = world::ScatterTree{};
    scene.scatter.push_back(l);
    ScatterPlacer p;
    place(p, scene);
    GIANT_CHECK(p.cells().size() == 4);
    // One cell a feed (the budget, one cell at least), nearest first.
    physics.points_per_feed = 1;
    const DVec3 camera{100.0, 10.0, 32.0};
    physics.feed_forest(scene, p, camera);
    GIANT_CHECK(physics.forest().cells().size() == 1 && physics.forest().serial({1, 0}) == p.find({1, 0})->serial);
    physics.feed_forest(scene, p, camera);
    GIANT_CHECK(physics.forest().cells().size() == 2 && physics.forest().serial({2, 0}) != 0); // 60 m against 68
    for (int i = 0; i < 4; ++i) physics.feed_forest(scene, p, camera);
    GIANT_CHECK(physics.forest().cells().size() == 4);
    u64 trees = 0;
    world::Transform foot;
    CellCoord cell{};
    for (const CellCoord c : p.cells()) {
        const ScatterPlacer::Cell* placed = p.find(c);
        for (const world::ScatterPoint& pt : placed->points) {
            const DVec3 at = pt.transform.position;
            if (trees++ == 0 || at.x < foot.position.x || (at.x == foot.position.x && at.z < foot.position.z)) {
                foot = pt.transform;
                cell = c;
            }
        }
    }
    ai::CommandResult r = run("get_physics");
    GIANT_CHECK(r.ok && r.value["trees"]["trunks"] == 0 && r.value["trees"]["fallen"] == 0);
    // A raycast finishes the trunks first, and names the tree it hits.
    r = run("raycast", {{"origin", {foot.position.x - 5.0, 3.0, foot.position.z}}, {"direction", {1, 0, 0}}});
    GIANT_CHECK(r.ok && r.value["hit"] == true && r.value["entity"] == 0 && r.value.contains("tree"));
    GIANT_CHECK(r.value["tree"]["cell"][0] == cell.x && r.value["tree"]["fallen"] == false);
    GIANT_CHECK(std::fabs(r.value["point"][0].get<double>() - (foot.position.x - 0.25)) < 0.01);
    r = run("get_physics");
    GIANT_CHECK(r.value["trees"]["trunks"] == trees && r.value["trees"]["waiting"] == 0);
    // Felled by impact; at rest within a few seconds, fixed where it lies.
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    r = run("impact", {{"point", {foot.position.x - 1.0, 1.0, foot.position.z}}, {"radius", 1.5}, {"impulse", 1e5}});
    GIANT_CHECK(r.ok && r.value["felled"] == 1 && r.value["loosened"] == 0 && r.value["trees"]["falling"] == 1);
    r = run("step_physics", {{"seconds", 6}});
    GIANT_CHECK(r.ok && r.value["trees"]["fallen"] == 1 && r.value["trees"]["falling"] == 0);
    const auto f = physics.fallen_trees().find(cell);
    GIANT_CHECK(f != physics.fallen_trees().end() && f->second.trees.size() == 1);
    if (f == physics.fallen_trees().end() || f->second.trees.empty()) return;
    const world::Transform lying = f->second.trees[0].at;
    // Drawn whole where it lies: the kit's pieces placed from its foot.
    SceneFrame frame;
    std::vector<CellCoord> redraw = physics.take_tree_redraw();
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees(),
                redraw);
    const auto drawn_at = [&](const DVec3& at) {
        for (const render::Instance& i : frame.instances) {
            if (i.mesh != render::k_no_mesh && length(i.position - at) < 1e-6) return true;
        }
        return false;
    };
    const DVec3 top = place(lying, pine.pieces[1].transform).position;
    GIANT_CHECK(std::fabs(top.y - lying.position.y) < 1.5 && drawn_at(top));
    GIANT_CHECK(!drawn_at(place(foot, pine.pieces[1].transform).position));
    // The cell placed again (another serial): its old fallen list is not
    // drawn on the new placement; fed, the tree stands where it stood, so it
    // stays fallen (the new placement's), drawn where it lies again.
    scene.scatter[0].items[0].material.roughness = 0.75f;
    place(p, scene);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees());
    GIANT_CHECK(physics.fallen_trees().at(cell).serial != p.find(cell)->serial);
    GIANT_CHECK(drawn_at(place(foot, pine.pieces[1].transform).position) && !drawn_at(top));
    physics.points_per_feed = 1u << 15;
    physics.feed_forest(scene, p, camera);
    GIANT_CHECK(physics.fallen_trees().at(cell).serial == p.find(cell)->serial);
    scene_frame(scene, *renderer, meshes, frame, nullptr, nullptr, &p, nullptr, {}, nullptr, &physics.fallen_trees(),
                physics.take_tree_redraw());
    GIANT_CHECK(drawn_at(top) && !drawn_at(place(foot, pine.pieces[1].transform).position));
}

GIANT_TEST("scatter trees: a blow far from the camera lays its trees down, with dust; stopping clears it") {
    PhysicsState physics;
    EditorState state;
    state.physics = &physics;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_physics_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); };
    world::Scene& scene = state.scene;
    flat(scene.terrain, 0, 0, 3, 0, 0.0f);
    world::ScatterLayer l = stones(16.0f);
    l.name = "pines";
    l.jitter = 0.0f;
    l.items[0].shape = world::Shape::cube;
    l.items[0].tree = world::ScatterTree{};
    scene.scatter.push_back(l);
    ScatterPlacer p;
    place(p, scene);
    // The camera 3 km off: no cell's trees are in reach, none given.
    physics.feed_forest(scene, p, {3000.0, 10.0, 32.0});
    GIANT_CHECK(physics.forest().cells().empty());
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    ai::CommandResult r = run("impact", {{"point", {96.0, 1.0, 32.0}}, {"radius", 30.0}, {"impulse", 1e6}});
    GIANT_CHECK(r.ok && r.value["felled"].get<u64>() > 4);
    GIANT_CHECK(r.value["trees"]["laid"] == r.value["felled"] && r.value["trees"]["trunks"] == 0);
    GIANT_CHECK(r.value["dust"]["puffs"].get<u64>() > 0);
    // Laid with their tops on the ground (flat here), about their feet.
    usize lying = 0;
    for (const auto& [c, f] : physics.fallen_trees()) {
        for (const sim::FallenTree& t : f.trees) {
            const Vec3 up = rotate(t.at.rotation, Vec3{0.0f, 1.0f, 0.0f});
            GIANT_CHECK(std::fabs(up.y) < 0.2f);
            ++lying;
        }
    }
    GIANT_CHECK(lying == r.value["felled"].get<usize>());
    // Dust drifts and rises while simulating; the cells stay while their trees lie.
    const double y = physics.dust().puffs().front().position.y;
    r = run("step_physics", {{"seconds", 1}});
    GIANT_CHECK(r.ok && physics.dust().puffs().front().position.y > y);
    physics.feed_forest(scene, p, {3000.0, 10.0, 32.0});
    physics.update(scene, {}, 0.0);
    GIANT_CHECK(!physics.forest().cells().empty());
    GIANT_CHECK(run("simulate", {{"on", false}}).ok);
    r = run("get_physics");
    GIANT_CHECK(r.value["dust"]["puffs"] == 0 && r.value["trees"]["fallen"] == 0 && r.value["trees"]["laid"] == 0);
}

GIANT_TEST("dent_ground: a crater while simulating, the bodies follow, saving sees the ground as it was, stopping puts it back") {
    PhysicsState physics;
    EditorState state;
    state.physics = &physics;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_physics_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); };
    world::Scene& scene = state.scene;
    flat(scene.terrain, 0, 0, 1, 0, 2.0f);
    const u64 version = scene.terrain.version({0, 0});
    GIANT_CHECK(!run("dent_ground", {{"point", {32.0, 0.0, 32.0}}, {"radius", 6.0}, {"depth", 3.0}}).ok); // not simulating
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    ai::CommandResult r = run("dent_ground", {{"point", {32.0, 0.0, 32.0}}, {"radius", 6.0}, {"depth", 3.0}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 1 && r.value["ground"]["dents"] == 1 && r.value["dust"]["puffs"] == 1);
    GIANT_CHECK(std::fabs(scene.terrain.height_at(32.0, 32.0).value() + 1.0) < 1e-4);
    // The ground's body follows: a ray down finds the crater's floor.
    r = run("raycast", {{"origin", {32.0, 20.0, 32.0}}, {"direction", {0, -1, 0}}});
    GIANT_CHECK(r.ok && r.value["hit"] == true && std::fabs(r.value["point"][1].get<double>() + 1.0) < 0.05);
    // A footprint, stretched.
    r = run("dent_ground", {{"point", {80.0, 0.0, 20.0}}, {"radius", 2.0}, {"depth", 0.5}, {"stretch", 3.0}, {"yaw", 90}});
    GIANT_CHECK(r.ok && r.value["tiles"] == 1 && r.value["ground"]["dents"] == 2);
    GIANT_CHECK(scene.terrain.height_at(80.0, 24.0).value() < 2.0 && scene.terrain.height_at(84.0, 20.0).value() == 2.0);
    // Saving: the ground as it was; then dented again.
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "giant_dent_test.scene.json";
    GIANT_CHECK(run("save_scene", {{"path", path.string()}}).ok);
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    world::Scene saved;
    GIANT_CHECK(!world::load_scene(text, saved).has_value());
    GIANT_CHECK(saved.terrain.height_at(32.0, 32.0).value() == 2.0);
    GIANT_CHECK(scene.terrain.height_at(32.0, 32.0).value() < 0.0);
    std::filesystem::remove(path);
    // Stopping: back exactly, the same version (nothing reads as edited).
    GIANT_CHECK(run("simulate", {{"on", false}}).ok);
    physics.update(scene, {}, 0.0);
    GIANT_CHECK(scene.terrain.height_at(32.0, 32.0).value() == 2.0 && scene.terrain.version({0, 0}) == version);
    r = run("get_physics");
    GIANT_CHECK(r.value["ground"]["dents"] == 0 && r.value["ground"]["tiles"] == 0);
}

GIANT_TEST("dent_ground: terrain edits, their undo and a new scene's undo never keep a dent for good") {
    PhysicsState physics;
    EditorState state;
    state.physics = &physics;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_terrain_commands(commands, state);
    register_physics_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); };
    world::Scene& scene = state.scene;
    flat(scene.terrain, 0, 0, 1, 0, 2.0f);
    const auto at = [&](double x, double z) { return state.scene.terrain.height_at(x, z).value(); };
    // A sculpt on a dented tile is made to the ground as authored; it takes the dent off that tile.
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    GIANT_CHECK(run("dent_ground", {{"point", {32.0, 0.0, 32.0}}, {"radius", 6.0}, {"depth", 3.0}}).ok);
    GIANT_CHECK(at(32.0, 32.0) < 0.0);
    GIANT_CHECK(run("sculpt_terrain", {{"op", "raise"}, {"points", {{4.0, 4.0}}}, {"radius", 3.0}, {"strength", 1.0}}).ok);
    GIANT_CHECK(at(32.0, 32.0) == 2.0 && at(4.0, 4.0) > 2.0);
    // Stopped, then undone: the sculpt goes, and no crater comes back.
    GIANT_CHECK(run("simulate", {{"on", false}}).ok);
    GIANT_CHECK(run("undo").ok);
    GIANT_CHECK(at(32.0, 32.0) == 2.0 && at(4.0, 4.0) == 2.0);
    // A dent, and a sculpt elsewhere (another tile): the dent stays, and stopping takes it away.
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    GIANT_CHECK(run("dent_ground", {{"point", {32.0, 0.0, 32.0}}, {"radius", 6.0}, {"depth", 3.0}}).ok);
    GIANT_CHECK(run("sculpt_terrain", {{"op", "raise"}, {"points", {{100.0, 32.0}}}, {"radius", 3.0}, {"strength", 1.0}}).ok);
    GIANT_CHECK(at(32.0, 32.0) < 0.0 && at(100.0, 32.0) > 2.0);
    GIANT_CHECK(run("simulate", {{"on", false}}).ok);
    GIANT_CHECK(at(32.0, 32.0) == 2.0 && at(100.0, 32.0) > 2.0); // at once, in the same batch
    GIANT_CHECK(run("undo").ok && at(100.0, 32.0) == 2.0 && at(32.0, 32.0) == 2.0);
    // A new scene while dented, then undone: the scene comes back as authored.
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    GIANT_CHECK(run("dent_ground", {{"point", {32.0, 0.0, 32.0}}, {"radius", 6.0}, {"depth", 3.0}}).ok);
    GIANT_CHECK(run("new_scene").ok);
    physics.update(state.scene, {}, 0.0);
    GIANT_CHECK(run("undo").ok);
    physics.update(state.scene, {}, 0.0);
    GIANT_CHECK(at(32.0, 32.0) == 2.0);
    // Too long a footprint is refused.
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    GIANT_CHECK(!run("dent_ground", {{"point", {32.0, 0.0, 32.0}}, {"radius", 100.0}, {"depth", 1.0}, {"stretch", 3.0}}).ok);
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
    // A tree item: its trunk given, shown, checked.
    r = f.run("add_scatter_layer", {{"name", "pines"}, {"items", {{{"shape", "cube"}, {"tree", {{"height", 12}}}}}}, {"spacing", 16}});
    GIANT_CHECK(r.ok && r.value["items"][0]["tree"]["height"] == 12.0 && r.value["items"][0]["tree"]["trunk_radius"] == 0.25);
    GIANT_CHECK(f.state.scene.scatter.back().items[0].tree.has_value());
    GIANT_CHECK(!f.run("set_scatter_layer", {{"name", "pines"}, {"items", {{{"tree", {{"height", 0.2}, {"trunk_radius", 1}}}}}}}).ok);
    r = f.run("set_scatter_layer", {{"name", "pines"}, {"items", {{{"shape", "cube"}, {"tree", nullptr}}}}});
    GIANT_CHECK(r.ok && r.value["items"][0]["tree"].is_null() && !f.state.scene.scatter.back().items[0].tree);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.scatter.back().items[0].tree.has_value());
    GIANT_CHECK(f.run("undo").ok && f.state.scene.scatter.size() == 1);
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

GIANT_TEST("keep_damage: what simulating broke stays through stopping, undo takes it back, clear_damage stands it up") {
    PhysicsState physics;
    EditorState state;
    state.physics = &physics;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_physics_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); };
    world::Scene& scene = state.scene;
    flat(scene.terrain, 0, 0, 3, 0, 0.0f);
    world::ScatterLayer l = stones(16.0f);
    l.name = "pines";
    l.jitter = 0.0f;
    l.items[0].shape = world::Shape::cube;
    l.items[0].tree = world::ScatterTree{};
    l.area = world::ScatterArea{128.0, 0.0, 256.0, 64.0}; // cells 2 and 3
    scene.scatter.push_back(l);
    world::Kit tower;
    tower.name = "tower";
    tower.breakable = true;
    for (int i = 0; i < 3; ++i) {
        world::KitPiece p;
        p.transform.position = {0.0, 0.5 + i, 0.0};
        tower.pieces.push_back(p);
    }
    scene.kits.push_back(tower);
    world::Entity e;
    e.shape = world::Shape::kit;
    e.kit = "tower";
    e.transform.position = {32.0, 0.0, 32.0};
    const world::EntityId t = scene.add(e);
    ScatterPlacer p;
    place(p, scene);
    const DVec3 camera{3000.0, 10.0, 32.0}; // far off: rubble and laid trees, no bodies
    const auto frame = [&] {
        physics.feed_forest(scene, p, camera);
        physics.update(scene, scene.take_changes(), 1.0 / 60.0);
    };
    frame();
    GIANT_CHECK(run("simulate", {{"on", true}}).ok);
    ai::CommandResult r = run("impact", {{"point", {32.6, 1.5, 32.0}}, {"radius", 0.3}, {"impulse", 6000.0}});
    GIANT_CHECK(r.ok && r.value["loosened"] == 2 && r.value["rubble"] == 2);
    r = run("impact", {{"point", {160.0, 1.0, 32.0}}, {"radius", 20.0}, {"impulse", 1e6}});
    const u64 felled = r.value["felled"].get<u64>();
    GIANT_CHECK(r.ok && felled > 2);
    GIANT_CHECK(run("dent_ground", {{"point", {100.0, 0.0, 32.0}}, {"radius", 6.0}, {"depth", 3.0}}).ok);
    const u64 dented = scene.terrain.version({1, 0});
    // Kept.
    r = run("keep_damage");
    GIANT_CHECK(r.ok && r.value["kits"] == 1 && r.value["pieces"] == 2 && r.value["trees"] == felled && r.value["tiles"] == 1);
    GIANT_CHECK(r.value["kept"]["kits"] == 1 && r.value["ground"]["dents"] == 0);
    GIANT_CHECK(scene.damage_in({0, 0}) != nullptr && scene.kept_kit(t) != nullptr);
    frame();
    r = run("get_physics");
    GIANT_CHECK(r.value["kept"]["pieces_down"] == 2 && r.value["kept"]["trees_down"] == felled && r.value["rubble"] == 0);
    // Stopped: it all stays (the pieces down, the trees down, the crater).
    GIANT_CHECK(run("simulate", {{"on", false}}).ok);
    frame();
    r = run("get_physics");
    GIANT_CHECK(r.value["broken_kits"] == 1 && r.value["kept"]["pieces_down"] == 2);
    GIANT_CHECK(r.value["trees"]["fallen"] == felled && r.value["kept"]["trees_down"] == felled);
    GIANT_CHECK(scene.terrain.height_at(100.0, 32.0).value() < -2.0 && scene.terrain.version({1, 0}) == dented);
    GIANT_CHECK(physics.piece_poses().count(t) == 1);
    // A scene file keeps it.
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value() && loaded.damage() == scene.damage());
    // Keeping again with nothing new changes nothing.
    r = run("keep_damage");
    GIANT_CHECK(r.ok && r.value["cells"] == 0 && r.value["tiles"] == 0);
    // Undone: whole, standing, the ground as it was; redone: kept again.
    GIANT_CHECK(run("undo").ok); // (the second keep recorded nothing)
    frame();
    r = run("get_physics");
    GIANT_CHECK(scene.damage().empty() && r.value["broken_kits"] == 0 && r.value["trees"]["fallen"] == 0);
    GIANT_CHECK(scene.terrain.height_at(100.0, 32.0).value() == 0.0);
    GIANT_CHECK(run("redo").ok);
    frame();
    r = run("get_physics");
    GIANT_CHECK(r.value["kept"]["pieces_down"] == 2 && r.value["trees"]["fallen"] == felled);
    GIANT_CHECK(scene.terrain.version({1, 0}) == dented);
    // Moved, the building stands whole; keeping drops its record.
    scene.modify(t)->transform.position.x = 34.0;
    frame();
    GIANT_CHECK(run("get_physics").value["broken_kits"] == 0);
    r = run("keep_damage");
    GIANT_CHECK(r.ok && r.value["kits"] == 0 && scene.kept_kit(t) == nullptr);
    scene.modify(t)->transform.position.x = 32.0;
    // Cleared: the trees stand; undone, down again.
    r = run("clear_damage");
    GIANT_CHECK(r.ok && r.value["cells"].get<u64>() >= 1 && scene.damage().empty());
    frame();
    GIANT_CHECK(run("get_physics").value["trees"]["fallen"] == 0);
    GIANT_CHECK(run("undo").ok);
    frame();
    GIANT_CHECK(run("get_physics").value["trees"]["fallen"] == felled);
}
