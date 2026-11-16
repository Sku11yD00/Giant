// The scene handed to the renderer by world cell: only what changed is rebuilt.
#include "../src/mesh_library.h"
#include "../src/scene_render.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"
#include "gltf_test_files.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

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

GIANT_TEST("scene cells: entities drawn at their poses, the posed one's cell rebuilt alone") {
    Cells c;
    if (c.renderer == nullptr) return;
    const world::EntityId a = c.add(10.0, 10.0);
    c.add(100.0, 10.0);
    c.build();
    const std::vector<render::CellRange> before = c.frame.cells;
    std::unordered_map<world::EntityId, world::Transform> poses;
    poses[a].position = {12.0, 3.0, 11.0}; // where the physics has it
    const std::vector<world::EntityId> redraw{a};
    scene_frame(c.scene, *c.renderer, *c.meshes, c.frame, nullptr, nullptr, nullptr, &poses, redraw);
    GIANT_CHECK(c.frame.cells_rebuilt == 1 && c.frame.cells[1].version == before[1].version);
    GIANT_CHECK(c.live()[0].position.y == 3.0 && c.live()[0].position.x == 12.0);
    GIANT_CHECK(c.scene.find(a)->transform.position.y == 0.0); // the scene keeps where it was put
    // The pose gone: drawn where the scene has it again.
    poses.clear();
    scene_frame(c.scene, *c.renderer, *c.meshes, c.frame, nullptr, nullptr, nullptr, &poses, redraw);
    GIANT_CHECK(c.frame.cells_rebuilt == 1 && c.live()[0].position.y == 0.0);
    GIANT_CHECK(c.frame.poses == nullptr);
}

GIANT_TEST("scene cells: a ragdoll is drawn where it lies, before its walker's pose, marked moving; let go, where the scene has it") {
    Cells c;
    if (c.renderer == nullptr) return;
    const world::EntityId a = c.add(10.0, 10.0);
    c.build();
    GIANT_CHECK(!c.live()[0].moves);
    std::unordered_map<world::EntityId, world::Transform> walking, lying;
    walking[a].position = {11.0, 0.0, 10.0};
    lying[a].position = {12.0, -0.5, 11.0};
    const std::vector<world::EntityId> redraw{a};
    scene_frame(c.scene, *c.renderer, *c.meshes, c.frame, nullptr, nullptr, nullptr, nullptr, redraw, nullptr, nullptr, {}, nullptr,
                nullptr, {}, nullptr, &walking, nullptr, &lying);
    GIANT_CHECK(c.frame.cells_rebuilt == 1 && c.live()[0].position.x == 12.0 && c.live()[0].position.y == -0.5);
    GIANT_CHECK(c.live()[0].moves); // kept out of the voxel world while it is one
    // Moved on (in place).
    lying[a].position = {12.5, -0.5, 11.0};
    scene_frame(c.scene, *c.renderer, *c.meshes, c.frame, nullptr, nullptr, nullptr, nullptr, {}, nullptr, nullptr, {}, nullptr,
                nullptr, redraw, nullptr, &walking, nullptr, &lying);
    GIANT_CHECK(c.live()[0].position.x == 12.5 && c.live()[0].moves);
    // Let go.
    lying.clear();
    walking.clear();
    scene_frame(c.scene, *c.renderer, *c.meshes, c.frame, nullptr, nullptr, nullptr, nullptr, redraw, nullptr, nullptr, {}, nullptr,
                nullptr, {}, nullptr, &walking, nullptr, &lying);
    GIANT_CHECK(c.live()[0].position.x == 10.0 && !c.live()[0].moves && c.frame.ragdolls == nullptr);
}

GIANT_TEST("scene cells: a material is two-sided for the renderer only while something uses it") {
    Cells c;
    if (c.renderer == nullptr) return;
    world::Entity leaf;
    leaf.transform.position = {10.0, 0.0, 10.0};
    leaf.material.two_sided = true;
    leaf.material.translucency = 0.4f;
    const world::EntityId id = c.scene.add(leaf);
    c.add(20.0, 20.0);
    c.build();
    const auto two_sided = [&] {
        usize n = 0;
        for (const render::Material& m : c.frame.materials) n += m.two_sided ? 1 : 0;
        return n;
    };
    GIANT_CHECK(c.frame.materials.size() == 2 && two_sided() == 1);
    for (const render::Material& m : c.frame.materials) {
        GIANT_CHECK(m.two_sided == (m.translucency == 0.4f));
    }
    c.scene.remove(id); // its material lingers, unused
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 2 && two_sided() == 0);
    c.scene.add(leaf); // used again: the same entry, two-sided again
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 2 && two_sided() == 1);
}

GIANT_TEST("scene cells: a material is cut out for the renderer only while something uses it") {
    Cells c;
    if (c.renderer == nullptr) return;
    world::Entity card;
    card.transform.position = {10.0, 0.0, 10.0};
    card.material.alpha_cutoff = 0.5f;
    const world::EntityId id = c.scene.add(card);
    c.add(20.0, 20.0);
    c.build();
    const auto cut = [&] {
        usize n = 0;
        for (const render::Material& m : c.frame.materials) n += m.alpha_cutoff > 0.0f ? 1 : 0;
        return n;
    };
    GIANT_CHECK(c.frame.materials.size() == 2 && cut() == 1);
    c.scene.remove(id); // its material lingers, unused
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 2 && cut() == 0);
    c.scene.add(card); // used again: the same entry, cut out again
    c.build();
    GIANT_CHECK(c.frame.materials.size() == 2 && cut() == 1);
}

GIANT_TEST("scene cells: a mesh in parts draws one instance a part, and all of them move in place") {
    Cells c;
    if (c.renderer == nullptr) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_scene_cells_tests";
    std::filesystem::create_directories(dir);
    const std::vector<u8> glb = geometry::testing::two_material_quad("n.png");
    std::ofstream(dir / "quad.glb", std::ios::binary)
        .write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const MeshLibrary::Loaded loaded = c.meshes->load((dir / "quad.glb").generic_string());
    GIANT_CHECK(loaded.mesh != nullptr && loaded.mesh->parts.size() == 2);
    if (loaded.mesh == nullptr || loaded.mesh->parts.size() != 2) return;
    world::Entity e;
    e.shape = world::Shape::mesh;
    e.mesh = loaded.mesh->path;
    e.transform.position = {10.0, 0.0, 10.0};
    e.material.base_color = {0.0f, 1.0f, 0.0f};
    e.material.sway = 0.5f; // how it moves in the wind and its sides: the entity's, on every part
    e.material.flutter = 0.25f;
    e.material.two_sided = true;
    e.material.translucency = 0.3f;
    e.material.canopy = 0.6f;
    const world::EntityId id = c.scene.add(e);
    c.add(20.0, 30.0); // a cube beside it, after it in the cell
    c.build();
    std::vector<render::Instance> now = c.live();
    GIANT_CHECK(now.size() == 3);
    if (now.size() != 3) return;
    // The first part with the entity's look, the second with the file's ("paint": plain white, non-metal).
    GIANT_CHECK(now[0].mesh == loaded.mesh->parts[0].id && now[1].mesh == loaded.mesh->parts[1].id);
    GIANT_CHECK(c.frame.materials[now[0].material].base_color == Vec3(0.0f, 1.0f, 0.0f) &&
                c.frame.materials[now[1].material].base_color == Vec3(1.0f, 1.0f, 1.0f) &&
                c.frame.materials[now[1].material].metallic == 0.0f);
    GIANT_CHECK(c.frame.materials[now[0].material].sway == 0.5f && c.frame.materials[now[1].material].sway == 0.5f &&
                c.frame.materials[now[1].material].two_sided && c.frame.materials[now[2].material].sway == 0.0f);
    GIANT_CHECK(c.frame.materials[now[1].material].flutter == 0.25f && c.frame.materials[now[1].material].translucency == 0.3f &&
                c.frame.materials[now[1].material].canopy == 0.6f);
    GIANT_CHECK(now[0].position.x == 10.0 && now[1].position.x == 10.0);
    // Moved (as a walker is): both parts go with it, the cell not laid out again, the cube untouched.
    std::unordered_map<world::EntityId, world::Transform> walkers;
    world::Transform at = e.transform;
    at.position.x = 12.0;
    walkers[id] = at;
    const std::vector<world::EntityId> moved = {id};
    scene_frame(c.scene, *c.renderer, *c.meshes, c.frame, nullptr, nullptr, nullptr, nullptr, {}, nullptr, nullptr, {},
                nullptr, nullptr, moved, nullptr, &walkers);
    now = c.live();
    // (In place: the renderer is told of the two instances that changed, not of a new cell.)
    GIANT_CHECK(now.size() == 3 && c.frame.cells.size() == 1 && c.frame.cells[0].prev_version != 0 &&
                c.frame.cells[0].changed_first == 0 && c.frame.cells[0].changed_count == 2);
    if (now.size() != 3) return;
    GIANT_CHECK(now[0].position.x == 12.0 && now[1].position.x == 12.0 && now[2].position.x == 20.0);
    GIANT_CHECK(now[0].mesh == loaded.mesh->parts[0].id && now[1].mesh == loaded.mesh->parts[1].id);
}

GIANT_TEST("scene cells: a placed kit with a piece in parts moves in place, each piece's instances where it belongs") {
    Cells c;
    if (c.renderer == nullptr) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_scene_cells_tests";
    std::filesystem::create_directories(dir);
    const std::vector<u8> glb = geometry::testing::two_material_quad("n.png");
    std::ofstream(dir / "kit_quad.glb", std::ios::binary)
        .write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const MeshLibrary::Loaded loaded = c.meshes->load((dir / "kit_quad.glb").generic_string());
    GIANT_CHECK(loaded.mesh != nullptr && loaded.mesh->parts.size() == 2);
    if (loaded.mesh == nullptr || loaded.mesh->parts.size() != 2) return;
    // A file not there (draws nothing), the quad in two parts, then a cube 3 m along x.
    world::Kit kit;
    kit.name = "stall";
    kit.pieces.resize(3);
    kit.pieces[0].shape = world::Shape::mesh;
    kit.pieces[0].mesh = (dir / "missing.glb").generic_string();
    kit.pieces[0].transform.position = {-3.0, 0.0, 0.0};
    kit.pieces[1].shape = world::Shape::mesh;
    kit.pieces[1].mesh = loaded.mesh->path;
    kit.pieces[2].shape = world::Shape::cube;
    kit.pieces[2].transform.position = {3.0, 0.0, 0.0};
    c.scene.kits.push_back(kit);
    world::Entity e;
    e.shape = world::Shape::kit;
    e.kit = "stall";
    e.transform.position = {10.0, 0.0, 10.0};
    const world::EntityId id = c.scene.add(e);
    c.add(20.0, 30.0);
    c.build();
    std::vector<render::Instance> now = c.live();
    GIANT_CHECK(now.size() == 4);
    if (now.size() != 4) return;
    GIANT_CHECK(now[0].mesh == loaded.mesh->parts[0].id && now[1].mesh == loaded.mesh->parts[1].id);
    GIANT_CHECK(now[0].position.x == 10.0 && now[1].position.x == 10.0 && now[2].position.x == 13.0);
    std::unordered_map<world::EntityId, world::Transform> walkers;
    world::Transform at = e.transform;
    at.position.x = 12.0;
    walkers[id] = at;
    const std::vector<world::EntityId> moved = {id};
    scene_frame(c.scene, *c.renderer, *c.meshes, c.frame, nullptr, nullptr, nullptr, nullptr, {}, nullptr, nullptr, {},
                nullptr, nullptr, moved, nullptr, &walkers);
    now = c.live();
    // In place: the three instances the kit draws, not a new cell; both parts with the quad, the cube beside.
    GIANT_CHECK(now.size() == 4 && c.frame.cells.size() == 1 && c.frame.cells[0].prev_version != 0 &&
                c.frame.cells[0].changed_first == 0 && c.frame.cells[0].changed_count == 3);
    if (now.size() != 4) return;
    GIANT_CHECK(now[0].position.x == 12.0 && now[1].position.x == 12.0 && now[2].position.x == 15.0 &&
                now[3].position.x == 20.0);
    GIANT_CHECK(now[0].mesh == loaded.mesh->parts[0].id && now[1].mesh == loaded.mesh->parts[1].id);
}

GIANT_TEST("mesh parts: triangles with no material are a part of their own; more than sixteen materials draw whole") {
    Cells c;
    if (c.renderer == nullptr) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_scene_cells_tests";
    std::filesystem::create_directories(dir);
    // The quad with its two "rust" triangles given no material (the same length of text).
    {
        const std::vector<u8> glb = geometry::testing::two_material_quad("n.png");
        std::string text(glb.begin(), glb.end());
        const std::string from = R"("indices":3,"material":1})";
        const usize at = text.find(from);
        GIANT_CHECK(at != std::string::npos);
        if (at == std::string::npos) return;
        text.replace(at, from.size(), R"("indices":3,"extras":{} })");
        std::ofstream(dir / "bare_quad.glb", std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    const MeshLibrary::Loaded bare = c.meshes->load((dir / "bare_quad.glb").generic_string());
    GIANT_CHECK(bare.mesh != nullptr && bare.mesh->parts.size() == 2);
    if (bare.mesh == nullptr || bare.mesh->parts.size() != 2) return;
    // Most triangles first: the two with no material (no name, no look, so the file has none
    // for entities to take), then "paint".
    GIANT_CHECK(bare.mesh->parts[0].name.empty() && bare.mesh->parts[0].triangles == 2 && !bare.mesh->parts[0].material &&
                bare.mesh->parts[1].name == "paint" && bare.mesh->parts[1].material && !bare.mesh->material &&
                !bare.mesh->whole_material);
    world::Entity e;
    e.shape = world::Shape::mesh;
    e.mesh = bare.mesh->path;
    e.material.base_color = {0.0f, 0.0f, 1.0f};
    c.scene.add(e);
    c.build();
    std::vector<render::Instance> now = c.live();
    GIANT_CHECK(now.size() == 2);
    if (now.size() != 2) return;
    GIANT_CHECK(c.frame.materials[now[0].material].base_color == Vec3(0.0f, 0.0f, 1.0f) &&
                c.frame.materials[now[1].material].base_color == Vec3(1.0f, 1.0f, 1.0f));

    // One triangle a material: sixteen is still in parts, seventeen is drawn whole (and says so).
    const auto many = [&](u32 materials) {
        std::vector<u8> bin;
        geometry::testing::append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 1, 1, 0});
        geometry::testing::append(bin, std::vector<u32>{0, 1, 2});
        std::string primitives, list;
        for (u32 m = 0; m < materials; ++m) {
            primitives += std::string(m == 0 ? "" : ",") + R"({"attributes":{"POSITION":0},"indices":1,"material":)" +
                          std::to_string(m) + "}";
            list += std::string(m == 0 ? "" : ",") + R"({"name":"m)" + std::to_string(m) + R"("})";
        }
        const std::string json =
            R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
            R"("meshes":[{"primitives":[)" + primitives + R"(]}],"materials":[)" + list + R"(],)"
            R"("buffers":[{"byteLength":48}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
            R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}]})";
        const std::vector<u8> glb = geometry::testing::make_glb(json, bin);
        const std::filesystem::path path = dir / ("many" + std::to_string(materials) + ".glb");
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
        return c.meshes->load(path.generic_string());
    };
    const MeshLibrary::Loaded sixteen = many(16);
    GIANT_CHECK(sixteen.mesh != nullptr && sixteen.mesh->parts.size() == 16 && sixteen.mesh->file_materials == 16);
    const MeshLibrary::Loaded seventeen = many(17);
    GIANT_CHECK(seventeen.mesh != nullptr && seventeen.mesh->parts.empty() && seventeen.mesh->file_materials == 17 &&
                seventeen.mesh->material && seventeen.mesh->whole_material == seventeen.mesh->material);
    if (seventeen.mesh == nullptr) return;
    bool told = false;
    for (const std::string& w : seventeen.mesh->warnings) told = told || w.find("too many to draw each as a part") != std::string::npos;
    GIANT_CHECK(told);
    world::Entity whole;
    whole.shape = world::Shape::mesh;
    whole.mesh = seventeen.mesh->path;
    whole.transform.position = {5.0, 0.0, 0.0};
    c.scene.add(whole);
    c.build();
    now = c.live();
    GIANT_CHECK(now.size() == 3 && now[2].mesh == seventeen.mesh->id);
}

GIANT_TEST("mesh parts: a file whose materials are woven through each other is warned of") {
    Cells c;
    if (c.renderer == nullptr) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_scene_cells_tests";
    std::filesystem::create_directories(dir);
    // A 64 x 64 grid, 8192 triangles; `checkered`: the squares' materials turn about, so every
    // corner is on a seam. Else two halves: one seam of 65 corners.
    const auto grid = [&](bool checkered) {
        const u32 n = 64;
        std::vector<float> positions;
        for (u32 j = 0; j <= n; ++j) {
            for (u32 i = 0; i <= n; ++i) {
                positions.insert(positions.end(), {static_cast<float>(i), 0.0f, static_cast<float>(j)});
            }
        }
        std::vector<u32> indices[2];
        for (u32 j = 0; j < n; ++j) {
            for (u32 i = 0; i < n; ++i) {
                const u32 a = j * (n + 1) + i;
                std::vector<u32>& to = indices[checkered ? (i + j) % 2 : (i < n / 2 ? 0 : 1)];
                to.insert(to.end(), {a, a + n + 1, a + n + 2, a, a + n + 2, a + 1});
            }
        }
        std::vector<u8> bin;
        geometry::testing::append(bin, positions);
        geometry::testing::append(bin, indices[0]);
        geometry::testing::append(bin, indices[1]);
        const usize p = positions.size() * 4, a = indices[0].size() * 4, b = indices[1].size() * 4;
        const std::string json =
            R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0},)"
            R"({"attributes":{"POSITION":0},"indices":2,"material":1}]}],)"
            R"("materials":[{"name":"dark"},{"name":"light"}],)"
            R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" + std::to_string(p) +
            R"(},{"buffer":0,"byteOffset":)" + std::to_string(p) + R"(,"byteLength":)" + std::to_string(a) +
            R"(},{"buffer":0,"byteOffset":)" + std::to_string(p + a) + R"(,"byteLength":)" + std::to_string(b) + R"(}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":)" + std::to_string(positions.size() / 3) +
            R"(,"type":"VEC3","min":[0,0,0],"max":[64,0,64]},)"
            R"({"bufferView":1,"componentType":5125,"count":)" + std::to_string(indices[0].size()) + R"(,"type":"SCALAR"},)"
            R"({"bufferView":2,"componentType":5125,"count":)" + std::to_string(indices[1].size()) + R"(,"type":"SCALAR"}]})";
        const std::vector<u8> glb = geometry::testing::make_glb(json, bin);
        const std::filesystem::path path = dir / (checkered ? "checkered.glb" : "halves.glb");
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
        return c.meshes->load(path.generic_string());
    };
    const auto seam_warnings = [](const MeshLibrary::Mesh& m) {
        usize told = 0;
        for (const std::string& w : m.warnings) told += w.find("where it meets other materials") != std::string::npos ? usize{1} : usize{0};
        return told;
    };
    const MeshLibrary::Loaded halves = grid(false);
    GIANT_CHECK(halves.mesh != nullptr && halves.mesh->parts.size() == 2 && seam_warnings(*halves.mesh) == 0);
    const MeshLibrary::Loaded checkered = grid(true);
    GIANT_CHECK(checkered.mesh != nullptr && checkered.mesh->parts.size() == 2 && seam_warnings(*checkered.mesh) == 2);
}
