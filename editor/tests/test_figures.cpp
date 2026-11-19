// People far from the camera drawn as figures (crowds plan, CR2): who is
// one, their slots, and the cell scene_frame draws them in.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_figures.h"
#include "../src/people_state.h"
#include "../src/pose_bakes.h"
#include "../src/scene_render.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include "giant/rhi/rhi.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

std::string write(const char* name, const std::vector<u8>& bytes) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_figure_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    std::unique_ptr<PoseBakes> poses;
    PeopleState bodies;
    PeopleFigures figures;
    ai::CommandRegistry commands;
    EditorState state;
    SceneFrame frame;
    std::string person; // a person of boxes, one part (clips "idle" and "raise")
    std::string arm;    // an arm in two materials: two parts (no clip "idle")
    DVec3 camera{};
    Fixture() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        renderer = render::Renderer::create(*device);
        meshes = std::make_unique<MeshLibrary>(renderer.get());
        poses = std::make_unique<PoseBakes>(renderer.get(), meshes.get());
        figures.poses = poses.get();
        figures.meshes = meshes.get();
        figures.settings.on = true;
        state.meshes = meshes.get();
        state.poses = poses.get();
        state.figures = &figures;
        register_people_commands(commands, state);
        person = write("person.glb", geometry::testing::skinned_person());
        geometry::testing::SkinOptions two;
        two.materials = true;
        arm = write("arm.glb", geometry::testing::skinned_arm(two));
        GIANT_CHECK(meshes->load(person).mesh != nullptr && meshes->load(arm).mesh != nullptr);
        world::PersonLook villager;
        villager.name = "villager";
        villager.mesh = person;
        villager.height = 1.1f;
        villager.material.base_color = {0.2f, 0.4f, 0.6f};
        state.scene.person_looks.push_back(villager);
    }
    ~Fixture() {
        poses.reset(); // its meshes leave the renderer first
        meshes.reset();
    }
    // `count` villagers in a row along +x from x = 10, 2 m apart (ids from 1).
    void row(u32 count) {
        for (u32 i = 0; i < count; ++i) {
            world::Person p;
            p.id = state.scene.people.empty() ? 1 : state.scene.people.back().id + 1;
            p.look = "villager";
            p.position = {10.0 + 2.0 * i, 0.0, 0.0};
            p.yaw = 90.0f;
            state.scene.people.push_back(p);
        }
    }
    // Frames: the poses handed over, then the figures (as main has them); every bake waited for.
    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            poses->finish();
            figures.update(state.scene, camera);
        }
    }
    void draw() {
        scene_frame(state.scene, *renderer, *meshes, frame, nullptr, nullptr, nullptr, nullptr, {}, nullptr, nullptr, {},
                    nullptr, nullptr, {}, nullptr, nullptr, nullptr, nullptr, nullptr, &figures.draws());
        // The cells (the scene's, the figures' and the holes) cover the instances end to end, as the renderer needs.
        usize covered = 0;
        bool tiled = true;
        for (const render::CellRange& c : frame.cells) {
            tiled = tiled && c.first == covered;
            covered += c.count;
        }
        GIANT_CHECK(tiled && covered == frame.instances.size());
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); }
    u32 drawn() const {
        return static_cast<u32>(std::count_if(frame.instances.begin(), frame.instances.end(), [](const render::Instance& i) {
            return i.mesh != render::k_no_mesh && i.moves;
        }));
    }
};

} // namespace

GIANT_TEST("figures: those within the distance are figures, made a few a frame; far, given an entity or gone, they are not") {
    Fixture f;
    if (f.renderer == nullptr) return;
    f.row(300); // x = 10 to 608
    f.figures.settings.distance = 200.0f;
    f.figures.settings.keep = 20.0f;
    f.figures.settings.made_a_frame = 10;
    f.figures.update(f.state.scene, f.camera);
    GIANT_CHECK(f.figures.stats().figures == 0); // (its look's pose is asked for first)
    f.frames(3);
    // The pose: the look's file standing in "idle", cut to the triangles set.
    GIANT_CHECK(f.poses->poses().size() == 1 && f.poses->poses()[0].clip == "idle" && f.poses->poses()[0].triangles == 1500);
    GIANT_CHECK(f.figures.stats().looks_ready == 1 && f.figures.stats().figures > 0 && f.figures.stats().figures <= 30);
    const u32 before = f.figures.stats().figures;
    f.frames(1);
    GIANT_CHECK(f.figures.stats().figures == before + 10); // a few a frame
    f.frames(20);
    // Everyone within 200 m: x = 10 to 200.
    GIANT_CHECK(f.figures.stats().figures == 96 && f.figures.stats().made == 96 && f.figures.stats().slots == 256);
    GIANT_CHECK(f.figures.draws().slots.size() == 256 && f.figures.draws().versions.size() == 4);
    GIANT_CHECK(f.figures.figure_of(96) != nullptr && f.figures.figure_of(97) == nullptr);
    const world::Transform* third = f.figures.figure_of(3);
    GIANT_CHECK(third != nullptr && third->position.x == 14.0 && third->scale.y == 1.1f);
    // One drawn stays a little farther than one is made: the camera 15 m back keeps all, 30 m back lets five go.
    f.camera = {-15.0, 0.0, 0.0};
    f.frames(3);
    GIANT_CHECK(f.figures.stats().figures == 96);
    f.camera = {-30.0, 0.0, 0.0};
    f.frames(3);
    GIANT_CHECK(f.figures.stats().figures == 91 && f.figures.figure_of(91) != nullptr && f.figures.figure_of(92) == nullptr);
    // The camera elsewhere: those there are figures now, the others no longer.
    f.camera = {700.0, 0.0, 0.0};
    f.frames(12);
    GIANT_CHECK(f.figures.stats().figures == 55 && f.figures.figure_of(1) == nullptr && f.figures.figure_of(300) != nullptr);
    f.camera = {};
    f.frames(20);
    GIANT_CHECK(f.figures.stats().figures == 96);

    // Those given an entity are figures no longer, that frame: before the list comes round to them
    // (one person of it looked at a frame here).
    f.figures.bodies = &f.bodies;
    f.figures.settings.scanned_a_frame = 1;
    f.bodies.settings.most = 5;
    for (int i = 0; i < 10; ++i) f.bodies.update(f.state.scene, f.camera, nullptr);
    GIANT_CHECK(f.bodies.stats().embodied == 5);
    u32 both = 0;
    for (world::PersonId id = 1; id <= 96; ++id) both += f.bodies.holds(id) && f.figures.figure_of(id) != nullptr ? 1u : 0u;
    GIANT_CHECK(both == 5);
    f.frames(1);
    both = 0;
    for (world::PersonId id = 1; id <= 96; ++id) both += f.bodies.holds(id) && f.figures.figure_of(id) != nullptr ? 1u : 0u;
    GIANT_CHECK(both == 0 && f.figures.stats().figures == 91 && f.figures.stats().removed >= 5);
    // A person removed from the scene: its figure goes as the slots are looked through (the list
    // would not find it: it is not in it).
    GIANT_CHECK(f.figures.figure_of(50) != nullptr);
    std::erase_if(f.state.scene.people, [](const world::Person& p) { return p.id == 50; });
    f.frames(1);
    GIANT_CHECK(f.figures.figure_of(50) == nullptr && f.figures.stats().figures == 90);
    f.figures.settings.scanned_a_frame = 1024;
    f.frames(3); // (and those with an entity are not made again while they have one)
    GIANT_CHECK(f.figures.stats().figures == 90);
    // No more than `most`; the rest wait.
    f.figures.settings.most = 20;
    f.frames(3);
    GIANT_CHECK(f.figures.stats().figures == 20 && f.figures.stats().waiting == 70);
    // Held back (a loaded game's people being put back), or off: nobody is a figure.
    f.figures.hold = true;
    f.frames(1);
    GIANT_CHECK(f.figures.stats().figures == 0 && f.figures.draws().slots.empty());
    f.figures.hold = false;
    f.frames(5);
    GIANT_CHECK(f.figures.stats().figures == 20);
    f.figures.settings.on = false;
    f.frames(1);
    GIANT_CHECK(f.figures.stats().figures == 0 && f.figures.draws().slots.empty() && f.figures.figure_of(10) == nullptr);
}

GIANT_TEST("figures: drawn by scene_frame in chunks, cells of their own among the scene's; one moved re-sends its chunk alone") {
    Fixture f;
    if (f.renderer == nullptr) return;
    // A scene with things of its own, in cells apart.
    for (int i = 0; i < 3; ++i) {
        world::Entity box;
        box.shape = world::Shape::cube;
        box.transform.position = {-500.0 * (i + 1), 0.0, 0.0};
        GIANT_CHECK(f.state.scene.add(box) != 0);
    }
    f.row(100);
    f.frames(6);
    GIANT_CHECK(f.figures.stats().figures == 100 && f.figures.stats().slots == 256);
    f.draw();
    const render::MeshId pose = f.poses->poses()[0].meshes[0];
    GIANT_CHECK(pose != render::k_no_mesh && f.drawn() == 100);
    // Where a slot's first instance is in the frame, and its cell (64 slots a chunk).
    struct Found {
        std::optional<render::CellRange> cell; // (by value: the frame's cells are made again each draw)
        u32 at = 0;
    };
    const auto find = [&](world::PersonId id) {
        Found found;
        const world::Transform* t = f.figures.figure_of(id);
        if (t == nullptr) return found;
        for (const render::CellRange& c : f.frame.cells) {
            for (u32 k = 0; k < c.count; ++k) {
                const render::Instance& i = f.frame.instances[c.first + k];
                if (i.mesh != render::k_no_mesh && i.moves && i.position == t->position) {
                    found.cell = c;
                    found.at = c.first + k;
                    return found;
                }
            }
        }
        return found;
    };
    const Found first = find(1), late = find(100);
    GIANT_CHECK(first.cell && late.cell);
    if (!first.cell || !late.cell) return;
    GIANT_CHECK(first.cell->first != late.cell->first);
    // Two chunks hold them: 64 slots, and the 36 used of the next (its other slots holes).
    GIANT_CHECK(first.cell->count == 64 && late.cell->count == 64 && first.at == first.cell->first);
    const render::Instance one = f.frame.instances[first.at];
    GIANT_CHECK(one.mesh == pose && one.position.x == 10.0 && one.scale.x == 1.1f);
    GIANT_CHECK(one.material < f.frame.materials.size() && f.frame.materials[one.material].base_color.z == 0.6f);
    GIANT_CHECK(f.frame.instances[late.cell->first + 36].mesh == render::k_no_mesh);
    const std::vector<render::CellRange> before = f.frame.cells;
    // Nothing changed: every cell as it was.
    f.frames(2);
    f.draw();
    const auto same = [](const render::CellRange& a, const render::CellRange& b) {
        return a.first == b.first && a.count == b.count && a.version == b.version;
    };
    GIANT_CHECK(std::equal(before.begin(), before.end(), f.frame.cells.begin(), f.frame.cells.end(), same));
    // One person elsewhere: its chunk is sent again where it was, every other cell is as it was.
    f.state.scene.people[7].position.z = 3.0;
    f.frames(1);
    f.draw();
    GIANT_CHECK(f.frame.cells.size() == before.size());
    u32 changed = 0;
    for (usize c = 0; c < before.size() && c < f.frame.cells.size(); ++c) {
        GIANT_CHECK(before[c].first == f.frame.cells[c].first && before[c].count == f.frame.cells[c].count);
        changed += before[c].version != f.frame.cells[c].version ? 1u : 0u;
    }
    GIANT_CHECK(changed == 1 && find(8).cell && f.frame.instances[first.cell->first + 7].position.z == 3.0);
    GIANT_CHECK(f.drawn() == 100);
    // A scene cell built again in a frame figures changed too: both are right after.
    f.state.scene.people[70].position.z = 4.0;
    world::Entity more;
    more.shape = world::Shape::sphere;
    more.transform.position = {-500.0, 2.0, 0.0};
    GIANT_CHECK(f.state.scene.add(more) != 0);
    f.frames(1);
    f.draw();
    GIANT_CHECK(f.drawn() == 100 && find(71).cell && f.frame.instances[find(71).at].position.z == 4.0);
    GIANT_CHECK(std::count_if(f.frame.instances.begin(), f.frame.instances.end(), [](const render::Instance& i) {
                    return i.mesh != render::k_no_mesh && !i.moves;
                }) == 4);
    // A chunk whose first slot is free: its holes stand by its figures (a cell's instances are kept
    // as floats about its first), not at the world's origin.
    std::erase_if(f.state.scene.people, [](const world::Person& p) { return p.id == 65; });
    f.frames(2);
    f.draw();
    GIANT_CHECK(f.figures.figure_of(65) == nullptr && f.drawn() == 99);
    const Found next = find(66);
    GIANT_CHECK(next.cell.has_value());
    if (!next.cell) return;
    const render::Instance hole = f.frame.instances[next.cell->first];
    GIANT_CHECK(hole.mesh == render::k_no_mesh && hole.position.x >= 10.0 && hole.position.x <= 210.0);

    // More than the slots there were: more are kept (doubled), the chunks drawn before staying as they were.
    const u64 kept = find(1).cell ? find(1).cell->version : 0;
    f.row(300); // (ids 101 on: put in a row beside the first, 1 m apart)
    for (usize i = 99; i < f.state.scene.people.size(); ++i) {
        f.state.scene.people[i].position = {10.0 + static_cast<double>(i - 99), 0.0, 10.0};
    }
    f.frames(8);
    GIANT_CHECK(f.figures.stats().figures == 99 + 300 && f.figures.stats().slots == 512);
    f.draw();
    GIANT_CHECK(f.drawn() == 399 && find(1).cell && find(1).cell->version != 0);
    GIANT_CHECK(std::any_of(f.frame.cells.begin(), f.frame.cells.end(), [&](const render::CellRange& c) { return c.version == kept; }));

    // A second look, in two parts: its chunk takes two instances a slot, each part with its own material.
    world::PersonLook soldier;
    soldier.name = "soldier";
    soldier.mesh = f.arm;
    soldier.material.base_color = {0.9f, 0.1f, 0.1f};
    f.state.scene.person_looks.push_back(soldier);
    world::Person p;
    p.id = 1000;
    p.look = "soldier";
    p.position = {5.0, 0.0, 5.0};
    f.state.scene.people.push_back(p);
    f.frames(12);
    GIANT_CHECK(f.figures.stats().figures == 400 && f.figures.stats().looks_ready == 2);
    // (A file without the clip stands in its rest pose.)
    GIANT_CHECK(f.poses->poses().size() == 2 && f.poses->poses()[1].clip.empty());
    f.draw();
    GIANT_CHECK(f.drawn() == 401);
    const Found two = find(1000);
    GIANT_CHECK(two.cell.has_value());
    if (!two.cell) return;
    GIANT_CHECK(two.cell->count >= 128); // (and room to grow: it outgrew its place)
    const render::Instance sleeve = f.frame.instances[two.at]; // (copies: the frame is made again below)
    const render::Instance sword = f.frame.instances[two.at + 1];
    GIANT_CHECK(sword.mesh != render::k_no_mesh && sword.mesh != sleeve.mesh && sword.material != sleeve.material &&
                f.frame.materials[sleeve.material].base_color.x == 0.9f);
    // A person given another look is drawn as that one.
    f.state.scene.people[0].look = "soldier";
    f.frames(2);
    f.draw();
    GIANT_CHECK(f.drawn() == 402 && find(1).cell && f.frame.instances[find(1).at].mesh == sleeve.mesh);
    // Two looks' names changed places (the same number of looks): everyone is still drawn, each as its name's look.
    // (Nobody's figure goes for it, whichever of the two is compared first: twice, a frame apart.)
    const u64 removed = f.figures.stats().removed;
    std::swap(f.state.scene.person_looks[0].name, f.state.scene.person_looks[1].name);
    f.frames(4);
    f.draw();
    GIANT_CHECK(f.figures.stats().figures == 400 && f.drawn() == 2 + 398 * 2 && f.frame.instances[find(1).at].mesh == pose);
    std::swap(f.state.scene.person_looks[0].name, f.state.scene.person_looks[1].name);
    f.frames(5);
    f.draw();
    GIANT_CHECK(f.figures.stats().figures == 400 && f.drawn() == 402 && f.figures.stats().removed == removed);
    // A look given another file while its people are figures: they draw nothing until its pose is
    // baked, then that (the one-part person in place of the two-part arm).
    f.state.scene.person_looks[1].mesh = f.person;
    f.frames(8);
    f.draw();
    GIANT_CHECK(f.figures.stats().figures == 400 && f.drawn() == 400 && f.frame.instances[find(1000).at].mesh == pose);
    // The material table made again (a material lost its last user, none may linger): the figures are drawn as before.
    f.frame.compact_unused_over = 0;
    f.state.scene.person_looks[1].material.base_color = {0.1f, 0.9f, 0.1f};
    f.frames(4);
    f.draw();
    GIANT_CHECK(f.drawn() == 400 && f.frame.materials[f.frame.instances[find(1000).at].material].base_color.y == 0.9f);
    u32 counted = 0;
    for (const u32 u : f.frame.users) counted += u;
    GIANT_CHECK(counted == 404); // (each figure's instance and the scene's four things, once)
    // Nobody a figure: nothing of them is drawn, and no material of theirs is counted.
    f.figures.settings.on = false;
    f.frames(1);
    f.draw();
    GIANT_CHECK(f.drawn() == 0);
    u32 users = 0;
    for (const u32 u : f.frame.users) users += u;
    GIANT_CHECK(users == 4); // (the scene's four things)
}

GIANT_TEST("figures: settings by command; another pose, or the poses forgotten, and the looks are baked again") {
    Fixture f;
    if (f.renderer == nullptr) return;
    f.row(30);
    const auto told = f.run("set_figure_settings");
    GIANT_CHECK(told.ok && told.value["on"] == true && told.value["distance"] == 400.0 && told.value["most"] == 4096 &&
                told.value["clip"] == "idle" && told.value["triangles"] == 1500 && told.value["figures"] == 0);
    GIANT_CHECK(!f.run("set_figure_settings", {{"most", 16385}}).ok && !f.run("set_figure_settings", {{"distance", 0.5}}).ok &&
                !f.run("set_figure_settings", {{"triangles", 10}}).ok);
    f.frames(6);
    GIANT_CHECK(f.run("set_figure_settings").value["figures"] == 30 && f.run("set_figure_settings").value["looks_ready"] == 1);
    // Another clip (one the file has): another pose, the figures back once it is baked.
    GIANT_CHECK(f.run("set_figure_settings", {{"clip", "raise"}, {"triangles", 100}}).ok);
    f.figures.update(f.state.scene, f.camera);
    GIANT_CHECK(f.figures.stats().figures == 0 && f.figures.stats().looks_ready == 0);
    f.frames(6);
    GIANT_CHECK(f.figures.stats().figures == 30 && f.poses->poses().size() == 2 && f.poses->poses()[1].clip == "raise" &&
                f.poses->poses()[1].triangles == 100);
    f.draw();
    GIANT_CHECK(f.drawn() == 30);
    // The poses forgotten (clear_baked_poses): no figure keeps a mesh that is gone; they are baked and drawn again.
    f.poses->clear();
    f.figures.update(f.state.scene, f.camera);
    f.draw();
    GIANT_CHECK(f.drawn() == 0 && f.figures.stats().figures == 30); // (kept: drawn again once the pose is back)
    f.frames(6);
    f.draw();
    GIANT_CHECK(f.drawn() == 30 && f.poses->poses().size() == 1);
    // The look's file imported again: its pose is baked again, the figures drawn with the new meshes.
    const render::MeshId old = f.poses->poses()[0].meshes[0];
    GIANT_CHECK(f.meshes->reload(f.person).mesh != nullptr);
    f.poses->update();
    f.figures.update(f.state.scene, f.camera);
    f.draw();
    GIANT_CHECK(f.drawn() == 0 && f.renderer->mesh(old) == nullptr);
    f.frames(4);
    f.draw();
    GIANT_CHECK(f.drawn() == 30);
    // A look changed: its figures are made again from the new one.
    f.state.scene.person_looks[0].height = 2.0f;
    f.frames(6);
    const world::Transform* one = f.figures.figure_of(1);
    GIANT_CHECK(one != nullptr && one->scale.y == 2.0f && f.figures.stats().figures == 30);
    f.draw();
    GIANT_CHECK(f.drawn() == 30 && std::all_of(f.frame.instances.begin(), f.frame.instances.end(), [](const render::Instance& i) {
        return i.mesh == render::k_no_mesh || i.scale.y == 2.0f;
    }));
    // Another scene: nothing of the last is kept.
    f.state.scene = world::Scene{};
    f.frames(2);
    GIANT_CHECK(f.figures.stats().figures == 0 && f.figures.stats().made == 0 && f.figures.draws().slots.empty());
}
