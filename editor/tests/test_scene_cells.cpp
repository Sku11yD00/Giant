// The scene handed to the renderer by world cell: only what changed is rebuilt.
#include "../src/mesh_library.h"
#include "../src/scene_render.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

using namespace giant;
using namespace giant::editor;

namespace {
struct Cells {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    world::Scene scene;
    SceneFrame frame;
    Cells() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        renderer = render::Renderer::create(*device);
        meshes = std::make_unique<MeshLibrary>(renderer.get());
    }
    world::EntityId add(double x, double z, Vec3 color = {0.8f, 0.8f, 0.8f}) {
        world::Entity e;
        e.transform.position = {x, 0.0, z};
        e.material.base_color = color;
        return scene.add(e);
    }
    void build() { scene_frame(scene, *renderer, *meshes, frame); }
    // The instances that draw, in list order (holes left out).
    std::vector<render::Instance> live() const {
        std::vector<render::Instance> out;
        for (const render::Instance& i : frame.instances) {
            if (i.mesh != render::k_no_mesh) out.push_back(i);
        }
        return out;
    }
};
} // namespace

GIANT_TEST("scene cells: a change rebuilds its cell only, in place") {
    Cells c;
    if (c.renderer == nullptr) return;
    const world::EntityId a = c.add(10.0, 10.0);
    c.add(20.0, 30.0);
    c.add(100.0, 10.0);  // the next cell along x
    c.add(-10.0, -70.0); // cell (-1, -2), first: cells go by z, then x
    c.build();
    GIANT_CHECK(c.frame.cells.size() == 3 && c.frame.instances.size() == 4 && c.frame.cells_rebuilt == 3);
    GIANT_CHECK(c.frame.cells[0].count == 1 && c.frame.cells[1].count == 2 && c.frame.cells[2].count == 1);
    GIANT_CHECK(c.frame.instances[0].position.z == -70.0);
    const std::vector<render::CellRange> before = c.frame.cells;

    c.build(); // nothing changed
    GIANT_CHECK(c.frame.cells_rebuilt == 0);
    GIANT_CHECK(c.frame.cells[1].version == before[1].version);

    c.scene.modify(a)->transform.position.x = 12.0; // within its cell
    c.build();
    GIANT_CHECK(c.frame.cells_rebuilt == 1);
    GIANT_CHECK(c.frame.cells[0].version == before[0].version && c.frame.cells[2].version == before[2].version);
    GIANT_CHECK(c.frame.cells[1].version != before[1].version);
    GIANT_CHECK(c.frame.instances[1].position.x == 12.0); // id order within the cell
    GIANT_CHECK(c.frame.instances.size() == 4);
}

GIANT_TEST("scene cells: moving between cells, removing and hiding") {
    Cells c;
    if (c.renderer == nullptr) return;
    const world::EntityId a = c.add(10.0, 10.0);
    const world::EntityId b = c.add(100.0, 10.0);
    c.build();
    GIANT_CHECK(c.frame.cells.size() == 2);
    c.scene.modify(a)->transform.position.x = 110.0; // into b's cell
    c.build();
    GIANT_CHECK(c.live().size() == 2 && c.live()[0].position.x == 110.0); // a's id is lower
    c.scene.remove(b);
    c.build();
    GIANT_CHECK(c.live().size() == 1 && c.live()[0].position.x == 110.0);
    c.scene.modify(a)->visible = false;
    c.build();
    GIANT_CHECK(c.live().empty());
    c.scene.remove(a);
    c.build();
    GIANT_CHECK(c.frame.cells.empty() && c.frame.instances.empty());
}

GIANT_TEST("scene cells: a kit change rebuilds the cells placing it, and unused materials go") {
    Cells c;
    if (c.renderer == nullptr) return;
    world::Kit kit;
    kit.name = "post";
    kit.pieces.push_back({});
    c.scene.kits.push_back(kit);
    world::Entity post;
    post.shape = world::Shape::kit;
    post.kit = "post";
    post.transform.position = {10.0, 0.0, 10.0};
    c.scene.add(post);
    c.add(100.0, 10.0, {1.0f, 0.0f, 0.0f});
    const world::EntityId green = c.add(200.0, 10.0, {0.0f, 1.0f, 0.0f});
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 3 && c.frame.cells.size() == 3);
    const std::vector<render::CellRange> before = c.frame.cells;

    c.scene.find_kit("post")->pieces[0].transform.scale = {2.0f, 2.0f, 2.0f};
    c.build();
    GIANT_CHECK(c.frame.cells_rebuilt == 1 && c.frame.cells[0].version != before[0].version);
    GIANT_CHECK(c.frame.cells[1].version == before[1].version);
    GIANT_CHECK(c.frame.instances[0].scale.x == 2.0f);

    // The last green instance goes: its entry stays, unused (no rebuild of
    // every cell for it)...
    c.scene.modify(green)->material.base_color = {1.0f, 0.0f, 0.0f};
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 3 && c.frame.cells_rebuilt == 1);
    // ...until unused entries pass the limit: the table is made again.
    c.frame.compact_unused_over = 0;
    c.scene.modify(green)->material.base_color = {0.0f, 0.0f, 1.0f};
    c.build();
    c.scene.modify(green)->material.base_color = {1.0f, 1.0f, 0.0f};
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 5); // two unused, three used
    c.scene.modify(green)->material.base_color = {1.0f, 0.0f, 0.0f};
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 2);
    for (const render::Instance& i : c.frame.instances) {
        GIANT_CHECK(i.material < c.frame.materials.size());
    }
    GIANT_CHECK(c.frame.materials[c.frame.instances[2].material].base_color.x == 1.0f);
}

GIANT_TEST("scene cells: a new scene starts over") {
    Cells c;
    if (c.renderer == nullptr) return;
    c.add(10.0, 10.0);
    c.build();
    world::Scene other;
    world::Entity e;
    e.transform.position = {500.0, 0.0, 500.0};
    other.add(e);
    other.take_changes(); // even with nothing left to take
    c.scene = other;
    c.build();
    GIANT_CHECK(c.frame.instances.size() == 1 && c.frame.instances[0].position.x == 500.0);
}

GIANT_TEST("scene cells: a new cell with only decals is laid out") {
    Cells c;
    if (c.renderer == nullptr) return;
    c.add(10.0, 10.0);
    c.build();
    world::Entity stamp;
    stamp.shape = world::Shape::decal;
    stamp.transform.position = {200.0, 0.0, 200.0};
    c.scene.add(stamp);
    c.build();
    GIANT_CHECK(c.frame.decals.size() == 1 && c.frame.cells.size() == 2);
    GIANT_CHECK(c.frame.cells[0].count == 0 || c.frame.cells[1].count == 0); // the decal cell, empty
}

GIANT_TEST("scene cells: cells coming and going leave the others where they are") {
    Cells c;
    if (c.renderer == nullptr) return;
    std::vector<world::EntityId> ids;
    for (int x = 0; x < 6; ++x) {
        for (int k = 0; k < 3; ++k) ids.push_back(c.add(10.0 + 64.0 * x + k, 10.0));
    }
    c.build();
    GIANT_CHECK(c.frame.cells.size() == 6 && c.frame.instances.size() == 18);
    const std::vector<render::CellRange> before = c.frame.cells;
    const auto unchanged = [&](usize cell) {
        for (const render::CellRange& r : c.frame.cells) {
            if (r.first == before[cell].first && r.count == before[cell].count && r.version == before[cell].version) {
                return true;
            }
        }
        return false;
    };
    // A cell goes: a hole of its size, the others as they were.
    c.scene.remove_many(std::vector<world::EntityId>{ids[3], ids[4], ids[5]});
    c.build();
    GIANT_CHECK(c.frame.instances.size() == 18 && c.live().size() == 15);
    for (const usize k : {0u, 2u, 3u, 4u, 5u}) GIANT_CHECK(unchanged(k));
    // A new cell of the same size takes the hole.
    for (int k = 0; k < 3; ++k) c.add(10.0 + k, 500.0);
    c.build();
    GIANT_CHECK(c.frame.instances.size() == 18 && c.live().size() == 18);
    for (const usize k : {0u, 2u, 3u, 4u, 5u}) GIANT_CHECK(unchanged(k));
    // A cell that grows moves to the end, with room to grow there.
    c.add(10.0 + 64.0 * 2 + 5, 10.0);
    c.build();
    GIANT_CHECK(c.live().size() == 19 && c.frame.instances.size() > 19);
    for (const usize k : {0u, 3u, 4u, 5u}) GIANT_CHECK(unchanged(k));
    const usize size = c.frame.instances.size();
    c.add(10.0 + 64.0 * 2 + 6, 10.0); // within the room it has now
    c.build();
    GIANT_CHECK(c.live().size() == 20 && c.frame.instances.size() == size);
    // The cells cover the list end to end.
    u32 next = 0;
    for (const render::CellRange& r : c.frame.cells) {
        GIANT_CHECK(r.first == next);
        next += r.count;
    }
    GIANT_CHECK(next == c.frame.instances.size());
}

GIANT_TEST("scene cells: too many holes and the list is laid out again") {
    Cells c;
    if (c.renderer == nullptr) return;
    c.frame.holes_allowed = 4;
    std::vector<world::EntityId> ids;
    for (int x = 0; x < 4; ++x) {
        for (int k = 0; k < 3; ++k) ids.push_back(c.add(10.0 + 64.0 * x + k, 10.0));
    }
    c.build();
    c.scene.remove_many(std::vector<world::EntityId>{ids[0], ids[1], ids[2]});
    c.build();
    GIANT_CHECK(c.frame.instances.size() == 12); // 3 holes: kept
    c.scene.remove_many(std::vector<world::EntityId>{ids[3], ids[4], ids[5]});
    c.build();
    GIANT_CHECK(c.frame.instances.size() == 12); // 6: half the list, kept
    c.scene.remove_many(std::vector<world::EntityId>{ids[6], ids[7], ids[8]});
    c.build();
    GIANT_CHECK(c.frame.instances.size() == 3 && c.live().size() == 3); // 9: laid out again
}

GIANT_TEST("scene cells: an empty cell never sits inside a hole") {
    Cells c;
    if (c.renderer == nullptr) return;
    std::vector<world::EntityId> left, right;
    for (int k = 0; k < 10; ++k) left.push_back(c.add(10.0 + k, 10.0));
    world::Entity stamp;
    stamp.shape = world::Shape::decal;
    stamp.transform.position = {80.0, 0.0, 10.0}; // cell (1, 0): decals only
    c.scene.add(stamp);
    for (int k = 0; k < 10; ++k) right.push_back(c.add(140.0 + k, 10.0));
    c.build();
    const auto tiles = [&] {
        u32 next = 0;
        for (const render::CellRange& r : c.frame.cells) {
            if (r.first != next) return false;
            next += r.count;
        }
        return next == c.frame.instances.size();
    };
    GIANT_CHECK(tiles());
    c.scene.remove_many(left);
    c.build();
    GIANT_CHECK(tiles());
    c.scene.remove_many(right); // the holes either side of where the empty cell was join
    c.build();
    GIANT_CHECK(tiles() && c.frame.decals.size() == 1);
    c.add(20.0, 500.0);
    c.build();
    GIANT_CHECK(tiles());
}

GIANT_TEST("scene cells: a hole is sent once, and stays as it is while holes beside it come and go") {
    Cells c;
    if (c.renderer == nullptr) return;
    std::vector<std::vector<world::EntityId>> cells(4);
    for (int x = 0; x < 4; ++x) {
        for (int k = 0; k < 5; ++k) cells[static_cast<usize>(x)].push_back(c.add(10.0 + 64.0 * x + k, 10.0));
    }
    c.build();
    const auto range_at = [&](u32 first) {
        for (const render::CellRange& r : c.frame.cells) {
            if (r.first == first && r.count > 0) return r;
        }
        return render::CellRange{};
    };
    c.scene.remove_many(cells[1]); // [5, 10) becomes a hole
    c.build();
    const render::CellRange hole = range_at(5);
    GIANT_CHECK(hole.count == 5);
    c.scene.remove_many(cells[2]); // [10, 15) too, beside it
    c.build();
    const render::CellRange kept = range_at(5);
    GIANT_CHECK(kept.count == 5 && kept.version == hole.version);
    // A cell of 7 takes the first 7 of the 10: the piece it cut is new,
    // and no bigger than what was left of it.
    for (int k = 0; k < 7; ++k) c.add(10.0 + k, 600.0);
    c.build();
    const render::CellRange rest = range_at(12);
    GIANT_CHECK(rest.count == 3 && rest.version != kept.version);
    GIANT_CHECK(c.frame.instances.size() == 20 && c.live().size() == 17);
}

GIANT_TEST("scene cells: a small cell taking part of a big hole re-sends at most a piece of it") {
    Cells c;
    if (c.renderer == nullptr) return;
    std::vector<world::EntityId> big;
    for (int k = 0; k < 3000; ++k) big.push_back(c.add(10.0 + 0.01 * k, 10.0));
    c.add(100.0, 10.0);
    c.build();
    c.scene.remove_many(big);
    c.build();
    const std::vector<render::CellRange> before = c.frame.cells;
    for (int k = 0; k < 40; ++k) c.add(10.0 + k, 600.0);
    c.build();
    // Instances in ranges not exactly as before: what the renderer looks at.
    u32 looked = 0;
    for (const render::CellRange& r : c.frame.cells) {
        bool same = false;
        for (const render::CellRange& b : before) {
            same = same || (b.first == r.first && b.count == r.count && b.version == r.version);
        }
        if (!same) looked += r.count;
    }
    GIANT_CHECK(looked > 0 && looked <= 1024);
    GIANT_CHECK(c.live().size() == 41);
}
