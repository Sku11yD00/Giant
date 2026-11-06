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
    GIANT_CHECK(c.frame.cells.size() == 1 && c.frame.cells[0].count == 2 && c.frame.instances.size() == 2);
    GIANT_CHECK(c.frame.instances[0].position.x == 110.0); // a's id is lower
    c.scene.remove(b);
    c.build();
    GIANT_CHECK(c.frame.instances.size() == 1 && c.frame.instances[0].position.x == 110.0);
    c.scene.modify(a)->visible = false;
    c.build();
    GIANT_CHECK(c.frame.instances.empty() && c.frame.cells.size() == 1 && c.frame.cells[0].count == 0);
    c.scene.remove(a);
    c.build();
    GIANT_CHECK(c.frame.cells.empty());
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

    // The last green instance goes: the table is made again without it.
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
    GIANT_CHECK(c.frame.decals.size() == 1 && c.frame.cells.size() == 2 && c.frame.cells[1].count == 0);
}
