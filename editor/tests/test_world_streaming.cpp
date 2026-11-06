// Worlds streamed by cell: cells load around the camera, far ones unload,
// and edits are never lost by streaming.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <atomic>
#include <filesystem>
#include <fstream>

using namespace giant;
using namespace giant::editor;

namespace {

std::filesystem::path fresh_folder() {
    static std::atomic<int> n{0};
    const std::filesystem::path p = std::filesystem::temp_directory_path() /
                                    ("giant_world_test_" + std::to_string(std::chrono::steady_clock::now()
                                                                              .time_since_epoch()
                                                                              .count()) +
                                     "_" + std::to_string(n++));
    std::filesystem::remove_all(p);
    return p;
}

struct World {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    world::Scene scene;
    SceneFrame frame;
    WorldStreaming streaming;
    std::filesystem::path folder = fresh_folder();
    u64 generation = 1;

    World() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        renderer = render::Renderer::create(*device);
        meshes = std::make_unique<MeshLibrary>(renderer.get());
        streaming.settings.load_radius = 200.0;
        streaming.settings.unload_margin = 64.0;
        streaming.settings.unload_every = 1;
    }
    ~World() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    world::EntityId add(double x, double z, const char* name = "") {
        world::Entity e;
        e.name = name;
        e.transform.position = {x, 0.0, z};
        return scene.add(e);
    }
    void step(const DVec3& camera) {
        scene_frame(scene, *renderer, *meshes, frame);
        streaming.update(scene, generation, camera, frame);
    }
    // Until nothing is read, waiting or left to unload.
    void settle(double x, double z) {
        for (int i = 0; i < 50; ++i) {
            streaming.finish_reads();
            step({x, 10.0, z});
        }
    }
    void reopen() {
        ++generation;
        GIANT_CHECK(!streaming.open(folder, scene, generation).has_value());
    }
    usize named(const char* name) const {
        usize n = 0;
        for (const world::Entity& e : scene.entities()) n += e.name == name ? usize{1} : usize{0};
        return n;
    }
};

} // namespace

GIANT_TEST("world streaming: cells near the camera load, far ones unload") {
    World w;
    if (w.renderer == nullptr) return;
    const world::EntityId a = w.add(10.0, 10.0, "near");
    w.add(20.0, 30.0, "near");
    w.add(100.0, 10.0, "next");        // cell (1, 0)
    w.add(2570.0, 10.0, "far");        // cell (40, 0)
    w.add(2600.0, 20.0, "far");
    const auto saved = w.streaming.create(w.folder, w.scene, w.generation, {});
    GIANT_CHECK(saved.error.empty() && saved.cells_written == 3 && saved.cells == 3);
    GIANT_CHECK(std::filesystem::exists(w.folder / "world.json"));
    GIANT_CHECK(std::filesystem::exists(w.folder / "cells" / "40_0.cell.json"));

    w.reopen();
    GIANT_CHECK(w.scene.size() == 0 && w.scene.next_id() == 6);
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.size() == 3 && w.named("near") == 2 && w.named("next") == 1);
    GIANT_CHECK(w.scene.find(a) != nullptr); // ids are the saved ones
    GIANT_CHECK(w.streaming.stats().loaded == 2 && w.streaming.stats().entities_loaded == 3);

    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.scene.size() == 2 && w.named("far") == 2);
    GIANT_CHECK(w.streaming.stats().entities_unloaded == 3);
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 0);
}

GIANT_TEST("world streaming: cells with unsaved edits stay until saved") {
    World w;
    if (w.renderer == nullptr) return;
    const world::EntityId a = w.add(10.0, 10.0, "near");
    w.add(100.0, 10.0, "next");
    w.add(2570.0, 10.0, "far");
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.settle(10.0, 10.0);
    w.scene.modify(a)->name = "edited";
    const world::EntityId fresh = w.add(30.0, 5.0, "new"); // same cell
    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.named("edited") == 1 && w.named("new") == 1); // kept: unsaved
    GIANT_CHECK(w.named("next") == 0);                           // clean: unloaded
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 1);
    const auto saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.cells_written == 1);
    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.named("edited") == 0 && w.scene.size() == 1);

    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.find(a) != nullptr && w.scene.find(a)->name == "edited");
    GIANT_CHECK(w.scene.find(fresh) != nullptr);
    GIANT_CHECK(w.scene.next_id() > fresh);
}

GIANT_TEST("world streaming: an entity moved into a cell on disk keeps that cell's others") {
    World w;
    if (w.renderer == nullptr) return;
    const world::EntityId a = w.add(10.0, 10.0, "mover");
    const world::EntityId b = w.add(20.0, 10.0, "stayer");
    w.add(2570.0, 10.0, "far");
    w.add(2600.0, 20.0, "far");
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.size() == 2);
    // Moved into cell (40, 0), on disk, and saved at once: the far file
    // gains the mover and keeps its two.
    w.scene.modify(a)->transform.position = {2580.0, 0.0, 30.0};
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty());
    std::vector<world::Entity> far;
    std::ifstream file(w.folder / "cells" / "40_0.cell.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    GIANT_CHECK(!world::load_cell(text, {40, 0}, far).has_value() && far.size() == 3);
    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.scene.size() == 3 && w.named("mover") == 1 && w.named("far") == 2);
    GIANT_CHECK(std::filesystem::exists(w.folder / "cells" / "0_0.cell.json")); // the stayer's
    // Moved there unsaved, from near: the far cell loads at once to join it.
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.size() == 1);
    w.scene.modify(b)->transform.position = {2590.0, 0.0, 40.0};
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.size() == 4 && w.named("stayer") == 1 && w.named("far") == 2 && w.named("mover") == 1);
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 2); // the far cell and the emptied near one
    GIANT_CHECK(w.streaming.save(w.scene, {}).cells_removed == 1);
    GIANT_CHECK(!std::filesystem::exists(w.folder / "cells" / "0_0.cell.json"));
}

GIANT_TEST("world streaming: whole cells join the scene within the per-frame budget") {
    World w;
    if (w.renderer == nullptr) return;
    for (int i = 0; i < 30; ++i) w.add(1.0 + i, 1.0, "a");   // cell (0, 0)
    for (int i = 0; i < 30; ++i) w.add(65.0 + i, 1.0, "b");  // cell (1, 0)
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.streaming.settings.entities_per_frame = 40;
    w.step({10.0, 0.0, 10.0}); // starts both reads
    w.streaming.finish_reads();
    w.step({10.0, 0.0, 10.0});
    GIANT_CHECK(w.scene.size() == 30); // one cell: a second would pass the budget
    w.step({10.0, 0.0, 10.0});
    GIANT_CHECK(w.scene.size() == 60);
}

GIANT_TEST("world streaming: a bad cell file fails alone, and saving over it is refused") {
    World w;
    if (w.renderer == nullptr) return;
    w.add(10.0, 10.0, "good");
    w.add(100.0, 10.0, "bad");
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    {
        std::ofstream broken(w.folder / "cells" / "1_0.cell.json", std::ios::binary | std::ios::trunc);
        broken << "{ not json";
    }
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.named("good") == 1 && w.scene.size() == 1 && w.streaming.stats().failed == 1);
    w.add(110.0, 10.0, "into the bad cell");
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.find("could not be read") != std::string::npos);
}

GIANT_TEST("world streaming: open_world and save_world commands") {
    World w;
    if (w.renderer == nullptr) return;
    ai::CommandRegistry commands;
    EditorState state;
    state.world = &w.streaming;
    register_scene_commands(commands, state);
    const std::string folder = w.folder.generic_string();
    GIANT_CHECK(!commands.execute("save_world").ok); // no world, no folder
    GIANT_CHECK(commands.execute("create_entity", {{"name", "x"}, {"position", {5, 0, 5}}}).ok);
    const auto made = commands.execute("save_world", {{"folder", folder}});
    GIANT_CHECK(made.ok && made.value["cells_written"] == 1);
    GIANT_CHECK(!commands.execute("save_world", {{"folder", folder + "_other"}}).ok);
    GIANT_CHECK(commands.execute("save_world").ok);
    GIANT_CHECK(commands.execute("new_scene").ok);
    GIANT_CHECK(!commands.execute("save_world").ok); // the scene is not the world's now
    const auto opened = commands.execute("open_world", {{"folder", folder}});
    GIANT_CHECK(opened.ok && opened.value["cells"] == 1);
    GIANT_CHECK(!commands.can_undo() && !commands.can_redo()); // opening a file forgets the history
    GIANT_CHECK(state.scene.size() == 0);
    const auto info = commands.execute("get_world");
    GIANT_CHECK(info.ok && info.value["open"] == true && info.value["active"] == true && info.value["cells"] == 1);
    GIANT_CHECK(commands.execute("set_world_streaming", {{"load_radius", 500}}).ok);
    GIANT_CHECK(w.streaming.settings.load_radius == 500.0);
    GIANT_CHECK(!commands.execute("open_world", {{"folder", folder + "_missing"}}).ok);
}
