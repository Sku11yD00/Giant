// People far from the camera drawn as figures (crowds plan, CR2): who is
// one, their slots, and the cell scene_frame draws them in.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_figures.h"
#include "../src/people_routines.h"
#include "../src/people_state.h"
#include "../src/people_travel.h"
#include "../src/pose_bakes.h"
#include "../src/scene_render.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include "giant/rhi/rhi.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cmath>
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

Json at(double x, double y, double z) { return Json::array({x, y, z}); }

struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    std::unique_ptr<PoseBakes> poses;
    PeopleState bodies;
    PeopleFigures figures;
    PeopleTravel travel;     // (the figures know of these two once `town` is built)
    PeopleRoutines routines;
    double seconds = 0.0;    // simulated, of run 1
    std::string walker;      // three joints, a triangle each: clips "idle", "walk" (1.4 m along +z in 1 s), "run", "wave"
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
    // A road east from (0, 0, 50) to a mill 200 m along it, and a look "walker" of the walker's file
    // that waves at work (once: held at its end) and idles at rest; people travel and keep routines.
    void town() {
        state.travel = &travel;
        state.routines = &routines;
        register_scene_commands(commands, state);
        register_route_commands(commands, state);
        figures.travel = &travel;
        figures.routines = &routines;
        routines.settings.spread_minutes = 0.0f;
        walker = write("walker.glb", geometry::testing::skinned_walker());
        GIANT_CHECK(meshes->load(walker).mesh != nullptr);
        world::PersonLook look;
        look.name = "walker";
        look.mesh = walker;
        look.activities.push_back({"work", "wave", 1.0f, false});
        look.activities.push_back({"rest", "idle", 1.0f, true});
        state.scene.person_looks.push_back(look);
        GIANT_CHECK(run("create_route", {{"points", {at(0, 0, 50), at(200, 0, 50)}}, {"kind", "road"}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "mill"}, {"position", at(200, 0, 50)}, {"radius", 4.0}, {"tags", {"work"}}}).ok);
    }
    world::PersonId one(const char* look, double x, const char* routine = "") {
        const ai::CommandResult r =
            run("create_people", {{"people", {{{"look", look}, {"position", at(x, 0, 50)}, {"routine", routine}}}}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["ids"][0].get<world::PersonId>() : 0;
    }
    // Frames of the town simulating: `more` seconds over them, at the hour; the figures after the people, as main has them.
    void live(double more = 0.0, int count = 4, double hours = 3.0) {
        for (int i = 0; i < count; ++i) {
            seconds += more / count;
            travel.update(state.scene, 1, seconds);
            routines.update(state.scene, travel, hours);
            poses->finish();
            figures.update(state.scene, camera);
        }
    }
    // The mesh of a pose baked (its first part's): k_no_mesh when there is none such.
    render::MeshId baked(const std::string& file, const char* clip, float time, bool in_place) const {
        for (const PoseBakes::Pose& p : poses->poses()) {
            if (p.mesh == file && p.clip == clip && std::fabs(p.time - time) < 1e-4f && p.in_place == in_place &&
                p.state == PoseBakes::State::ready) {
                return p.meshes.front();
            }
        }
        return render::k_no_mesh;
    }
    // The mesh the one figure there is is drawn with (k_no_mesh: none is, or more than one).
    render::MeshId drawn_as() {
        draw();
        render::MeshId mesh = render::k_no_mesh;
        u32 count = 0;
        for (const render::Instance& i : frame.instances) {
            if (i.mesh == render::k_no_mesh || !i.moves) continue;
            mesh = i.mesh;
            ++count;
        }
        return count == 1 ? mesh : render::k_no_mesh;
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

GIANT_TEST("figures: one travelling is put along its way each frame, facing along it, in the pose of the walk it has come to") {
    Fixture f;
    if (f.renderer == nullptr) return;
    f.town();
    const world::PersonId id = f.one("walker", 0.0);
    f.live();
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_stand && f.figures.stats().walking == 0 && f.figures.stats().poses == 1);
    GIANT_CHECK(f.drawn_as() != render::k_no_mesh && f.drawn_as() == f.baked(f.walker, "idle", 0.0f, false));
    GIANT_CHECK(f.figures.kind_of(99) == PeopleFigures::k_no_kind && f.figures.yaw_of(99) == 0.0f);

    // Sent along the road at 1.4 m/s: the walk covers 1.4 m a round, so a round a second, eight poses to it.
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(200, 0, 50)}, {"speed", 1.4}}).ok);
    f.live();
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_walk && f.figures.stats().walking == 1 && f.figures.stats().moved == 1);
    GIANT_CHECK(std::fabs(f.figures.yaw_of(id) - 90.0f) < 1e-3f); // facing east, along its way
    f.live(0.3);
    const world::Transform* figure = f.figures.figure_of(id);
    GIANT_CHECK(figure != nullptr && std::fabs(figure->position.x - 0.42) < 1e-9 && figure->position.z == 50.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_walk + 2);
    f.live();
    GIANT_CHECK(f.drawn_as() != render::k_no_mesh && f.drawn_as() == f.baked(f.walker, "walk", 0.25f, true));
    // Its poses: standing and the walk's eight, asked for together when it set off, each in place.
    // Where nothing moves, nothing is sent again.
    GIANT_CHECK(f.figures.stats().poses == 9 && f.baked(f.walker, "walk", 0.875f, true) != render::k_no_mesh);
    const u64 still_at = f.figures.draws().versions[0];
    f.live();
    GIANT_CHECK(f.figures.draws().versions[0] == still_at && f.figures.stats().moved == 1);
    f.live(0.01, 1);
    GIANT_CHECK(f.figures.draws().versions[0] != still_at);
    // Half a round on: the pose half a round on; a whole round more, the same again.
    f.live(0.5 - 0.01, 1);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_walk + 6);
    f.live(1.0, 8);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_walk + 6 && f.figures.stats().poses == 9);
    GIANT_CHECK(f.drawn_as() == f.baked(f.walker, "walk", 0.75f, true));
    // The command tells of it.
    ai::CommandResult r = f.run("set_figure_settings", {{"id", id}});
    GIANT_CHECK(r.ok && r.value["figure"]["pose"] == "walk" && r.value["figure"]["walk_pose"] == 6 && r.value["walking"] == 1);
    GIANT_CHECK(std::fabs(r.value["figure"]["at"][0].get<double>() - 1.4 * 1.8) < 1e-9 && std::fabs(r.value["figure"]["yaw"].get<double>() - 90.0) < 1e-3);
    GIANT_CHECK(f.run("set_figure_settings", {{"id", 99}}).value["figure"].is_null());
    // Only so many are put on a frame, in turn.
    const world::PersonId second = f.one("walker", 0.0), third = f.one("walker", 0.0);
    GIANT_CHECK(f.run("send_person", {{"id", second}, {"to", at(200, 0, 50)}}).ok && f.run("send_person", {{"id", third}, {"to", at(200, 0, 50)}}).ok);
    f.live();
    GIANT_CHECK(f.figures.stats().walking == 3 && f.figures.stats().moved == 3);
    f.figures.settings.moved_a_frame = 2;
    f.figures.settings.scanned_a_frame = 1; // (the list would put them on too as it comes round)
    const auto behind = [&] {
        u32 count = 0;
        for (const world::PersonId one : {id, second, third}) {
            count += f.figures.figure_of(one)->position.x != f.travel.where(*world::find_person(f.state.scene.people, one)).position.x ? 1u : 0u;
        }
        return count;
    };
    f.live(0.1, 1);
    GIANT_CHECK(f.figures.stats().moved == 2 && behind() == 1); // (the third waits its turn)
    // The turn goes round: over three frames each is put on twice, the last two of them where they are now.
    const double were[3] = {f.figures.figure_of(id)->position.x, f.figures.figure_of(second)->position.x,
                            f.figures.figure_of(third)->position.x};
    f.live(0.1, 1);
    GIANT_CHECK(f.figures.stats().moved == 2 && behind() == 1);
    f.live(0.1, 1);
    GIANT_CHECK(behind() == 1 && f.figures.figure_of(id)->position.x != were[0] &&
                f.figures.figure_of(second)->position.x != were[1] && f.figures.figure_of(third)->position.x != were[2]);
    // The poses forgotten as they walk: they stand where they are, never drawn as nothing but for
    // the frames their look has no pose at all, then step again.
    f.figures.settings.moved_a_frame = 2048;
    f.poses->clear();
    f.live(0.0, 1);
    f.draw();
    GIANT_CHECK(f.figures.stats().poses == 0 && f.figures.stats().figures == 3 && f.drawn() == 0);
    f.live(0.0, 8);
    f.draw();
    GIANT_CHECK(f.figures.stats().poses == 9 && f.figures.stats().walking == 3 && f.drawn() == 3);
    // Its file imported again: its poses are baked again, and they walk on.
    GIANT_CHECK(f.meshes->load(f.walker).mesh != nullptr);
    f.live(0.5, 8);
    f.draw();
    GIANT_CHECK(f.figures.stats().walking == 3 && f.drawn() == 3 && f.figures.stats().poses == 9);
    GIANT_CHECK(f.figures.kind_of(id) >= PeopleFigures::k_first_walk && f.figures.kind_of(id) < PeopleFigures::k_first_activity);
    f.figures.settings.scanned_a_frame = 1024;
    // Arrived: it stands where it was sent, and is not put on any more.
    f.live(200.0);
    figure = f.figures.figure_of(id);
    GIANT_CHECK(figure != nullptr && figure->position.x == 200.0 && f.figures.kind_of(id) == PeopleFigures::k_stand);
    GIANT_CHECK(f.figures.stats().walking == 0 && f.figures.stats().moved == 0);

    // No clip to walk in: one travelling stands as it goes.
    r = f.run("set_figure_settings", {{"walk_clip", ""}});
    GIANT_CHECK(r.ok && r.value["walk_clip"] == "");
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(0, 0, 50)}, {"speed", 1.4}}).ok);
    f.live(0.0, 6);
    f.live(1.0);
    figure = f.figures.figure_of(id);
    GIANT_CHECK(figure != nullptr && std::fabs(figure->position.x - 198.6) < 1e-9 && f.figures.kind_of(id) == PeopleFigures::k_stand);
    GIANT_CHECK(f.figures.stats().walking == 1 && std::fabs(f.figures.yaw_of(id) + 90.0f) < 1e-3f);
    // A look whose file has no such clip: its people stand as they go too, by the stride set (a round
    // 1.4 m at height 1: the villager is 1.1 tall).
    GIANT_CHECK(f.run("set_figure_settings", {{"walk_clip", "walk"}, {"walk_poses", 4}, {"stride", 2.0}}).ok);
    std::erase_if(f.state.scene.people, [](const world::Person&) { return true; });
    f.live();
    const world::PersonId villager = f.one("villager", 0.0);
    f.live();
    GIANT_CHECK(f.run("send_person", {{"id", villager}, {"to", at(200, 0, 50)}, {"speed", 1.2}}).ok);
    f.live(0.0, 6);
    f.live(1.0); // 1.2 m gone of a round of 2.2: past half way, the third pose of four
    GIANT_CHECK(f.figures.kind_of(villager) == PeopleFigures::k_first_walk + 2 && f.figures.stats().figures == 1);
    f.live(0.0, 6);
    GIANT_CHECK(f.drawn_as() != render::k_no_mesh && f.drawn_as() == f.baked(f.person, "idle", 0.0f, false));
    // A rig whose hips are not found by name cannot be held in place: none of its walk is baked, it stands as it goes.
    world::PersonLook armed;
    armed.name = "armed";
    armed.mesh = f.arm;
    f.state.scene.person_looks.push_back(armed);
    GIANT_CHECK(f.run("set_figure_settings", {{"walk_clip", "bend"}}).ok);
    const world::PersonId arm = f.one("armed", 0.0);
    f.live(0.0, 6);
    GIANT_CHECK(f.run("send_person", {{"id", arm}, {"to", at(200, 0, 50)}}).ok);
    f.live(1.0, 8);
    GIANT_CHECK(f.figures.kind_of(arm) >= PeopleFigures::k_first_walk && f.figures.figure_of(arm)->position.x > 1.0);
    for (const PoseBakes::Pose& p : f.poses->poses()) GIANT_CHECK(p.mesh != f.arm || !p.in_place);
    // Settings out of range are refused.
    GIANT_CHECK(!f.run("set_figure_settings", {{"walk_poses", 1}}).ok && !f.run("set_figure_settings", {{"walk_poses", 17}}).ok);
    GIANT_CHECK(!f.run("set_figure_settings", {{"stride", 0.0}}).ok && !f.run("set_figure_settings", {{"moved_a_frame", 0}}).ok);
}

GIANT_TEST("figures: one at what its routine has it do is held in its look's pose for it; at the hidden activity it is not drawn") {
    Fixture f;
    if (f.renderer == nullptr) return;
    f.town();
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", {{{"from", 6}, {"activity", "work"}, {"at", "place"}, {"name", "mill"}, {"speed", 2.0}},
                                                                    {{"from", 12}, {"activity", "rest"}},
                                                                    {{"from", 16}, {"activity", "eat"}},
                                                                    {{"from", 20}, {"activity", "sleep"}}}}}).ok);
    const world::PersonId id = f.one("walker", 180.0, "day");
    // Sent to the mill, 20 m off: it walks there, a figure in the walk's poses, not yet at work.
    f.live(0.0, 6, 7.0);
    GIANT_CHECK(f.travel.trip(id) != nullptr && f.figures.stats().walking == 1 && f.figures.stats().acting == 0);
    f.live(4.0, 4, 7.0);
    GIANT_CHECK(f.figures.kind_of(id) >= PeopleFigures::k_first_walk && f.figures.kind_of(id) < PeopleFigures::k_first_activity);
    // There: held in the pose its look gives work, the end of a clip played once.
    f.live(10.0, 8, 7.0);
    GIANT_CHECK(f.travel.trip(id) == nullptr && f.figures.kind_of(id) == PeopleFigures::k_first_activity);
    GIANT_CHECK(f.figures.stats().acting == 1 && f.figures.stats().walking == 0);
    f.live(0.0, 4, 7.0);
    GIANT_CHECK(f.drawn_as() != render::k_no_mesh && f.drawn_as() == f.baked(f.walker, "wave", 0.8f, false));
    ai::CommandResult r = f.run("set_figure_settings", {{"id", id}});
    GIANT_CHECK(r.ok && r.value["figure"]["pose"] == "activity" && r.value["figure"]["activity"] == "work" && r.value["acting"] == 1);
    // Resting where it is: the look's pose for that (a looping clip's start).
    f.live(0.0, 6, 13.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_activity + 1 && f.figures.stats().acting == 1);
    GIANT_CHECK(f.drawn_as() == f.baked(f.walker, "idle", 0.0f, false));
    // An activity the look has no clip for: it stands.
    f.live(0.0, 6, 17.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_stand && f.figures.stats().acting == 0 && f.figures.stats().figures == 1);
    // Asleep: not drawn; with no activity hidden, it stands there.
    f.live(0.0, 6, 21.0);
    GIANT_CHECK(f.figures.figure_of(id) == nullptr && f.figures.stats().figures == 0 && f.figures.stats().hidden == 1);
    GIANT_CHECK(f.drawn_as() == render::k_no_mesh && f.run("set_figure_settings").value["hidden_now"] == 1);
    GIANT_CHECK(f.run("set_figure_settings", {{"hidden", ""}}).ok);
    f.live(0.0, 6, 21.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_stand && f.figures.stats().hidden == 0);
    // The look's activities changed: one held in a pose of them is in the pose the look gives now
    // (standing until the list has come round to it), or stands when it gives none.
    f.live(0.0, 6, 13.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_activity + 1);
    world::PersonLook& look = f.state.scene.person_looks[1];
    // (A clip's speed alone shows on entities only: nobody is put again.)
    const u64 as_was = f.figures.draws().versions[0];
    look.activities[1].speed = 2.0f;
    f.live(0.0, 6, 13.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_activity + 1 && f.figures.draws().versions[0] == as_was);
    look.activities.erase(look.activities.begin()); // ("rest" is its first now)
    f.live(0.0, 6, 13.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_first_activity && f.figures.stats().acting == 1);
    GIANT_CHECK(f.drawn_as() != render::k_no_mesh && f.drawn_as() == f.baked(f.walker, "idle", 0.0f, false));
    look.activities.clear();
    f.live(0.0, 6, 13.0);
    GIANT_CHECK(f.figures.kind_of(id) == PeopleFigures::k_stand && f.figures.stats().acting == 0);
    GIANT_CHECK(f.drawn_as() == f.baked(f.walker, "idle", 0.0f, false));
    // The simulation stopped: nobody is doing anything, everyone stands where the scene has them.
    for (int i = 0; i < 4; ++i) {
        f.travel.update(f.state.scene, 0, 0.0);
        f.routines.update(f.state.scene, f.travel, 13.0);
        f.frames(1);
    }
    const world::Transform* figure = f.figures.figure_of(id);
    GIANT_CHECK(figure != nullptr && figure->position.x == 180.0 && f.figures.kind_of(id) == PeopleFigures::k_stand);
}

GIANT_TEST("figures: with the poses nearly all taken, a standing pose is had before any other and room is kept for each look's") {
    Fixture f;
    if (f.renderer == nullptr) return;
    f.town(); // two looks: the villager and the walker
    // All but six of the poses that can be kept are taken by others.
    std::string error;
    for (u32 i = 0; i < PoseBakes::k_max_poses - 6; ++i) {
        GIANT_CHECK(f.poses->want(f.arm, "bend", 0.001f * static_cast<float>(i % 500 + 1), 3000 + i / 500, error) == i);
    }
    const world::PersonId walker = f.one("walker", 0.0);
    f.live();
    GIANT_CHECK(f.run("send_person", {{"id", walker}, {"to", at(200, 0, 50)}, {"speed", 1.4}}).ok);
    f.live(1.3, 8);
    // The walker's standing pose was had, and four of its walk's: the next would take the room kept for
    // the villager's. With part of the round it stands as it goes (not every other step).
    GIANT_CHECK(f.poses->poses().size() == PoseBakes::k_max_poses - 1 && f.figures.stats().poses == 5);
    GIANT_CHECK(f.baked(f.walker, "walk", 0.25f, true) != render::k_no_mesh && f.baked(f.walker, "walk", 0.5f, true) == render::k_no_mesh);
    GIANT_CHECK(f.figures.kind_of(walker) >= PeopleFigures::k_first_walk && f.figures.stats().walking == 1);
    GIANT_CHECK(f.drawn_as() != render::k_no_mesh && f.drawn_as() == f.baked(f.walker, "idle", 0.0f, false));
    // A villager comes: its standing pose takes the last room, and it is drawn.
    const world::PersonId villager = f.one("villager", 20.0);
    f.live();
    f.draw();
    GIANT_CHECK(f.poses->poses().size() == PoseBakes::k_max_poses && f.figures.stats().poses == 6 && f.figures.stats().looks_ready == 2);
    GIANT_CHECK(f.figures.figure_of(villager) != nullptr && f.drawn() == 2);
    // The poses cleared: there is room, and the walker steps.
    f.poses->clear();
    f.live(0.0, 8);
    GIANT_CHECK(f.figures.stats().poses == 10 && f.drawn_as() == render::k_no_mesh);
    f.draw();
    GIANT_CHECK(f.drawn() == 2 && f.baked(f.walker, "walk", 0.0f, true) != render::k_no_mesh);
}

GIANT_TEST("figures: one whose entity is taken away is a figure in that frame, where it stands, before the list comes round") {
    Fixture f;
    if (f.renderer == nullptr) return;
    GIANT_CHECK(!PeopleFigures::Settings{}.on); // (off until the crowd gate has measured them: the fixture turns them on)
    f.row(40); // x = 10 to 88
    f.frames(4);
    GIANT_CHECK(f.figures.stats().figures == 40);
    // Five are given entities: they are figures no longer.
    f.figures.bodies = &f.bodies;
    f.bodies.settings.most = 5;
    for (int i = 0; i < 6; ++i) {
        f.bodies.update(f.state.scene, f.camera, nullptr);
        f.frames(1);
    }
    std::vector<world::PersonId> held;
    f.bodies.held(held);
    GIANT_CHECK(held.size() == 5 && f.figures.stats().figures == 35 && f.figures.stats().taken_over == 0);
    for (const world::PersonId id : held) GIANT_CHECK(f.figures.figure_of(id) == nullptr);
    // Their entities taken away (no more may be held): each is a figure again in that same frame,
    // though the list would come round to one person of it a frame.
    f.figures.settings.scanned_a_frame = 1;
    f.bodies.settings.most = 0;
    f.bodies.update(f.state.scene, f.camera, nullptr);
    GIANT_CHECK(f.bodies.let_go().size() == 5 && f.bodies.stats().embodied == 0);
    f.frames(1);
    GIANT_CHECK(f.figures.stats().figures == 40 && f.figures.stats().taken_over == 5);
    for (const world::PersonId id : held) {
        const world::Transform* figure = f.figures.figure_of(id);
        const world::Person* person = world::find_person(f.state.scene.people, id);
        GIANT_CHECK(figure != nullptr && figure->position.x == person->position.x);
    }
    f.draw();
    GIANT_CHECK(f.drawn() == 40);
    // The next frame nobody was let go: nothing more is taken over.
    f.bodies.update(f.state.scene, f.camera, nullptr);
    f.frames(1);
    GIANT_CHECK(f.bodies.let_go().empty() && f.figures.stats().taken_over == 5 && f.figures.stats().figures == 40);
    // With as many figures as may be kept, one let go waits like anyone.
    f.bodies.settings.most = 5;
    for (int i = 0; i < 6; ++i) {
        f.bodies.update(f.state.scene, f.camera, nullptr);
        f.frames(1);
    }
    GIANT_CHECK(f.figures.stats().figures == 35);
    f.figures.settings.most = 35;
    f.bodies.settings.most = 0;
    f.bodies.update(f.state.scene, f.camera, nullptr);
    f.frames(1);
    GIANT_CHECK(f.figures.stats().figures == 35 && f.figures.stats().taken_over == 5);
}

GIANT_TEST("figures: one let go on its way walks on as a figure from where its entity was; one asleep is taken over by nobody") {
    Fixture f;
    if (f.renderer == nullptr) return;
    f.town();
    f.bodies.travel = &f.travel;
    f.figures.bodies = &f.bodies;
    f.bodies.settings.most = 0;
    f.camera = {0.0, 0.0, 50.0};
    // As main has them: where the people are, their entities, then the figures.
    const auto live = [&f](double more, int count, double hours) {
        for (int i = 0; i < count; ++i) {
            f.seconds += more / count;
            f.travel.update(f.state.scene, 1, f.seconds);
            f.routines.update(f.state.scene, f.travel, hours);
            f.bodies.update(f.state.scene, f.camera, nullptr);
            f.poses->finish();
            f.figures.update(f.state.scene, f.camera);
        }
    };
    const world::PersonId id = f.one("walker", 0.0);
    live(0.0, 4, 3.0);
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(200, 0, 50)}, {"speed", 1.4}}).ok);
    live(5.0, 10, 3.0);
    const world::Transform* figure = f.figures.figure_of(id);
    GIANT_CHECK(figure != nullptr && figure->position.x > 6.0 && figure->position.x < 7.5 && f.figures.stats().walking == 1);
    // Given an entity (nothing walks it here: it stands where it was made, its trip held there).
    f.bodies.settings.most = 1;
    live(2.0, 4, 3.0);
    const world::Entity* entity = f.state.scene.find(world::person_entity_id(id));
    GIANT_CHECK(entity != nullptr && f.figures.figure_of(id) == nullptr && f.figures.stats().figures == 0);
    if (entity == nullptr) return;
    const double stood = entity->transform.position.x;
    GIANT_CHECK(stood > 6.0 && stood < 10.0);
    // Its entity taken away: that frame it is a figure there, walking, and goes on from there.
    f.figures.settings.scanned_a_frame = 1;
    f.bodies.settings.most = 0;
    live(0.0, 1, 3.0);
    figure = f.figures.figure_of(id);
    GIANT_CHECK(f.bodies.let_go().size() == 1 && f.figures.stats().taken_over == 1 && f.figures.stats().walking == 1);
    GIANT_CHECK(figure != nullptr && std::fabs(figure->position.x - stood) < 1e-6 && figure->position.z == 50.0);
    GIANT_CHECK(f.figures.kind_of(id) >= PeopleFigures::k_first_walk && f.figures.kind_of(id) < PeopleFigures::k_first_activity);
    live(1.0, 2, 3.0);
    figure = f.figures.figure_of(id);
    GIANT_CHECK(figure != nullptr && std::fabs(figure->position.x - (stood + 1.4)) < 1e-6);

    // One asleep by the mill has no figure; its entity taken away, it still has none (it is not
    // drawn at that), until nothing is hidden and the list comes round.
    GIANT_CHECK(f.run("set_routine", {{"name", "night"}, {"entries", {{{"from", 0}, {"activity", "sleep"}}}}}).ok);
    const world::PersonId sleeper = f.one("walker", 180.0, "night");
    f.figures.settings.scanned_a_frame = 1024;
    f.camera = {180.0, 0.0, 50.0};
    live(0.0, 6, 3.0);
    GIANT_CHECK(f.figures.figure_of(sleeper) == nullptr && f.figures.stats().hidden == 1);
    f.bodies.settings.most = 1;
    live(0.0, 6, 3.0);
    GIANT_CHECK(f.bodies.holds(sleeper));
    f.bodies.settings.most = 0;
    live(0.0, 1, 3.0);
    GIANT_CHECK(f.bodies.let_go().size() == 1 && f.bodies.let_go()[0] == sleeper);
    GIANT_CHECK(f.figures.figure_of(sleeper) == nullptr && f.figures.stats().taken_over == 1);
    f.figures.settings.hidden = "";
    live(0.0, 4, 3.0);
    GIANT_CHECK(f.figures.figure_of(sleeper) != nullptr && f.figures.stats().taken_over == 1);
}
