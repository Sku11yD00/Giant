// Worlds streamed by cell: cells load around the camera, far ones unload,
// and edits are never lost by streaming.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>

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
    std::string text;
    {
        // Closed again at once: Windows cannot replace a file that is open.
        std::ifstream file(w.folder / "cells" / "40_0.cell.json", std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
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
    const WorldStreaming::Saved last = w.streaming.save(w.scene, {});
    GIANT_CHECK(last.error.empty() && last.cells_removed == 1);
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
    const auto opened = commands.execute("open_world", {{"folder", folder}, {"discard_unsaved", true}});
    GIANT_CHECK(opened.ok && opened.value["cells"] == 1);
    GIANT_CHECK(state.scene.size() == 0);
    const auto info = commands.execute("get_world");
    GIANT_CHECK(info.ok && info.value["open"] == true && info.value["active"] == true && info.value["cells"] == 1);
    GIANT_CHECK(commands.execute("set_world_streaming", {{"load_radius", 500}}).ok);
    GIANT_CHECK(w.streaming.settings.load_radius == 500.0);
    GIANT_CHECK(!commands.execute("open_world", {{"folder", folder + "_missing"}}).ok);
}

namespace {
// The editor's state and commands, with frames built and streamed as main does.
struct Editor {
    World w;
    ai::CommandRegistry commands;
    EditorState state;
    Editor() {
        state.world = &w.streaming;
        register_scene_commands(commands, state);
        register_terrain_commands(commands, state);
    }
    bool run(const char* name, const ai::Json& args = ai::Json::object()) { return commands.execute(name, args).ok; }
    bool script(const char* name, const ai::Json& args = ai::Json::object()) { // outside the history
        ai::ExecuteOptions options;
        options.record_history = false;
        return commands.execute(name, args, options).ok;
    }
    void settle(double x, double z) {
        for (int i = 0; i < 50; ++i) {
            w.streaming.finish_reads();
            scene_frame(state.scene, *w.renderer, *w.meshes, w.frame);
            w.streaming.update(state.scene, state.scene_generation, {x, 10.0, z}, w.frame);
        }
    }
    std::string folder() const { return w.folder.generic_string(); }
};

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}
} // namespace

GIANT_TEST("world streaming: undo and redo across new_scene never lose cell files") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entities", {{"entities", {{{"position", {5, 0, 5}}}, {{"position", {70, 0, 5}}}}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("new_scene"));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("new_scene"));
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.size() == 0); // paused: the scene is not the world's
    GIANT_CHECK(e.commands.undo());
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.size() == 2); // resumed
    // The world's scene comes back as it was left, cells loaded.
    GIANT_CHECK(e.commands.redo());
    GIANT_CHECK(e.commands.undo());
    GIANT_CHECK(e.state.scene.size() == 2);
    const auto saved = e.commands.execute("save_world");
    GIANT_CHECK(saved.ok && saved.value["cells_removed"] == 0 && saved.value["cells_written"] == 0 &&
                saved.value["cells"] == 2);
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.size() == 2);
    // open_world is undoable too.
    GIANT_CHECK(e.commands.undo() && e.state.scene.size() == 0 && !e.w.streaming.active(e.state.scene_generation));
}

GIANT_TEST("world streaming: an id two cell files share is loaded twice, under a new id") {
    World w;
    if (w.renderer == nullptr) return;
    w.add(10.0, 10.0, "a");
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    write_file(w.folder / "cells" / "0_0.cell.json", R"({"format": "giant-cell", "version": 1, "cell": [0, 0],
        "entities": [{"id": 1, "name": "a1"}, {"id": 2, "name": "a2"}]})");
    write_file(w.folder / "cells" / "1_0.cell.json", R"({"format": "giant-cell", "version": 1, "cell": [1, 0],
        "entities": [{"id": 2, "name": "b2", "position": [70, 0, 5]}]})");
    write_file(w.folder / "world.json", R"({"format": "giant-world", "version": 1, "next_id": 2,
        "cells": [[0, 0], [1, 0], [2, 0]]})"); // a stale next id, and a cell whose file is gone
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.size() == 3 && w.named("a2") == 1 && w.named("b2") == 1);
    GIANT_CHECK(w.streaming.stats().ids_changed == 1 && w.streaming.stats().failed == 0);
    GIANT_CHECK(w.streaming.stats().cells == 2); // the missing file counts as an empty cell
    const world::EntityId fresh = w.add(20.0, 20.0, "new");
    GIANT_CHECK(fresh > 2);
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty());
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.size() == 4 && w.streaming.stats().ids_changed == 0);
}

GIANT_TEST("world streaming: saving merges a cell parsed but not yet added") {
    World w;
    if (w.renderer == nullptr) return;
    w.add(10.0, 10.0, "old");
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.step({10.0, 10.0, 10.0}); // starts the read
    w.streaming.finish_reads(); // parsed, waiting for the next frame
    GIANT_CHECK(w.streaming.stats().waiting == 1 && w.scene.size() == 0);
    w.add(30.0, 30.0, "new"); // into that cell
    GIANT_CHECK(w.streaming.save(w.scene, {}).error.empty());
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.named("old") == 1 && w.named("new") == 1);
}

GIANT_TEST("world streaming: commands guard what streaming cannot see") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_kit", {{"name", "hut"}, {"pieces", {{{"shape", "cube"}}}}}));
    GIANT_CHECK(e.run("create_entity", {{"shape", "kit"}, {"kit", "hut"}, {"position", {2570, 0, 5}}}));
    GIANT_CHECK(e.run("create_entity", {{"position", {5, 0, 5}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(!e.run("save_world", {{"folder", e.folder() + "_2"}})); // a world is open
    GIANT_CHECK(e.run("new_scene"));
    GIANT_CHECK(!e.run("save_world", {{"folder", e.folder() + "_2"}})); // still open, paused
    GIANT_CHECK(!e.run("open_world", {{"folder", e.folder()}})); // the world is open, its scene replaced
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}, {"discard_unsaved", true}}));
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.size() == 1);
    GIANT_CHECK(e.run("delete_entities", {{"ids", {e.state.scene.entities()[0].id}}}));
    // The far cell places the kit, unseen: deleting it is refused.
    GIANT_CHECK(!e.run("delete_kit", {{"name", "hut"}}));
    // Unsaved edits: another open is refused unless they are given up.
    GIANT_CHECK(!e.run("open_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}, {"discard_unsaved", true}}));
    GIANT_CHECK(e.run("close_world"));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder() + "_2"}}));
    std::error_code error;
    std::filesystem::remove_all(e.folder() + "_2", error);
}

GIANT_TEST("world streaming: a moved entity survives undo and redo of new_scene once, not twice") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entities", {{"entities", {{{"name", "a"}, {"position", {10, 0, 10}}},
                                                        {{"name", "b"}, {"position", {100, 0, 10}}}}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    world::EntityId a = 0;
    for (const auto& x : e.state.scene.entities()) a = x.name == "a" ? x.id : a;
    GIANT_CHECK(e.run("set_entity", {{"id", a}, {"position", {110, 0, 10}}})); // into b's cell
    GIANT_CHECK(e.run("new_scene"));
    GIANT_CHECK(e.commands.undo() && e.commands.redo() && e.commands.undo());
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(!std::filesystem::exists(e.w.folder / "cells" / "0_0.cell.json")); // emptied
    e.settle(10.0, 10.0);
    usize as = 0;
    for (const auto& x : e.state.scene.entities()) as += x.name == "a" ? usize{1} : usize{0};
    GIANT_CHECK(as == 1 && e.w.streaming.stats().ids_changed == 0);
}

GIANT_TEST("world streaming: ids saved since never go out again after undo and redo of open_world") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entity", {{"name", "one"}, {"position", {10, 0, 10}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    const ai::CommandResult made = e.commands.execute("create_entity", {{"name", "B"}, {"position", {20, 0, 20}}});
    GIANT_CHECK(made.ok);
    const world::EntityId b = made.value["id"];
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(e.commands.undo());                      // B goes (unsaved: its file still has it)
    GIANT_CHECK(e.commands.undo() && e.commands.redo()); // out of the world and back
    const ai::CommandResult next = e.commands.execute("create_entity", {{"name", "C"}, {"position", {30, 0, 30}}});
    GIANT_CHECK(next.ok);
    const world::EntityId c = next.value["id"];
    e.settle(10.0, 10.0);
    GIANT_CHECK(c > b && e.state.scene.find(b) == nullptr && e.state.scene.find(c) != nullptr);
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 1); // B's removal, C's arrival
}

GIANT_TEST("world streaming: cells near the camera are read before cells left behind") {
    World w;
    if (w.renderer == nullptr) return;
    for (int z = -3; z <= 3; ++z)
        for (int x = -3; x <= 3; ++x) w.add(x * 64.0 + 10.0, z * 64.0 + 10.0, "home");
    w.add(20005.0, 10.0, "far");
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.streaming.settings.reads_in_flight = 1;
    w.step({0.0, 10.0, 0.0});
    for (int i = 0; i < 3; ++i) {
        w.streaming.finish_reads();
        w.step({20005.0, 10.0, 10.0});
    }
    GIANT_CHECK(w.named("far") == 1);
}

namespace {
usize count_named(const world::Scene& scene, const char* name) {
    usize n = 0;
    for (const world::Entity& e : scene.entities()) n += e.name == name ? usize{1} : usize{0};
    return n;
}
} // namespace

GIANT_TEST("world streaming: a cell saved after open_world survives undo and redo of open_world") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entity", {{"name", "one"}, {"position", {10, 0, 10}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.run("create_entity", {{"name", "C"}, {"position", {100, 0, 10}}})); // new cell (1, 0)
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(e.commands.undo());                      // C leaves the scene, not the file
    GIANT_CHECK(e.commands.undo() && e.commands.redo()); // out of the world and back
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.w.streaming.stats().cells == 2 && count_named(e.state.scene, "C") == 0);
    GIANT_CHECK(e.commands.redo()); // C again
    GIANT_CHECK(count_named(e.state.scene, "C") == 1);
    GIANT_CHECK(e.run("create_entity", {{"name", "D"}, {"position", {110, 0, 10}}}));
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    GIANT_CHECK(count_named(e.state.scene, "C") == 1 && count_named(e.state.scene, "D") == 1);
}

GIANT_TEST("world streaming: redoing an entity saved elsewhere since does not copy it") {
    for (const bool swap : {true, false}) {
        Editor e;
        if (e.w.renderer == nullptr) return;
        GIANT_CHECK(e.run("create_entities", {{"entities", {{{"name", "a"}, {"position", {10, 0, 10}}},
                                                            {{"name", "b"}, {"position", {100, 0, 10}}}}}}));
        GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
        GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
        e.settle(10.0, 10.0);
        const ai::CommandResult made = e.commands.execute("create_entity", {{"name", "X"}, {"position", {20, 0, 10}}});
        GIANT_CHECK(made.ok);
        const world::EntityId x = made.value["id"];
        GIANT_CHECK(e.run("save_world"));
        GIANT_CHECK(e.run("set_entity", {{"id", x}, {"position", {110, 0, 10}}})); // into b's cell
        GIANT_CHECK(e.run("save_world"));
        if (swap) {
            GIANT_CHECK(e.commands.undo() && e.commands.undo()); // the move, the create
            GIANT_CHECK(e.commands.undo() && e.commands.redo()); // open_world
        } else {
            e.settle(5000.0, 10.0); // every cell unloads
            GIANT_CHECK(e.state.scene.size() == 0);
            GIANT_CHECK(e.commands.undo() && e.commands.undo());
        }
        GIANT_CHECK(e.commands.redo()); // the create: X back in cell (0, 0), its file in (1, 0)
        e.settle(10.0, 10.0);
        GIANT_CHECK(count_named(e.state.scene, "X") == 1 && e.w.streaming.stats().ids_changed == 0);
    }
}

GIANT_TEST("world streaming: a world made from a new scene keeps streaming across its undo and redo") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("new_scene"));
    GIANT_CHECK(e.run("create_entity", {{"name", "a"}, {"position", {10, 0, 10}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.commands.undo() && e.commands.undo()); // the create, new_scene
    GIANT_CHECK(e.commands.redo() && e.commands.redo());
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.w.streaming.active(e.state.scene_generation));
    GIANT_CHECK(count_named(e.state.scene, "a") == 1);
    GIANT_CHECK(e.run("create_entity", {{"name", "b"}, {"position", {20, 0, 10}}}));
    GIANT_CHECK(e.run("save_world"));
}

GIANT_TEST("world streaming: a scene a script put there is never kept as an undo step's") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entity", {{"name", "one"}, {"position", {10, 0, 10}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.run("create_entity", {{"name", "two"}, {"position", {20, 0, 10}}})); // unsaved
    GIANT_CHECK(e.script("new_scene"));
    GIANT_CHECK(e.script("create_entity", {{"name", "s"}}));
    GIANT_CHECK(e.commands.undo() && e.commands.undo()); // two (not this scene's: nothing), open_world
    GIANT_CHECK(e.commands.redo() && e.commands.redo()); // open_world, two
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.w.streaming.active(e.state.scene_generation));
    GIANT_CHECK(count_named(e.state.scene, "one") == 1 && count_named(e.state.scene, "two") == 1 &&
                count_named(e.state.scene, "s") == 0);
}

GIANT_TEST("world streaming: a save the stored bookkeeping does not know of is merged, not lost") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entity", {{"name", "one"}, {"position", {10, 0, 10}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.run("create_entity", {{"name", "two"}, {"position", {20, 0, 10}}}));
    GIANT_CHECK(e.run("create_entity", {{"name", "far"}, {"position", {150, 0, 10}}})); // a new cell
    GIANT_CHECK(e.run("save_world"));
    // A script replaces the world's scene, so the open_world step keeps its
    // world side from before the save.
    GIANT_CHECK(e.script("new_scene"));
    GIANT_CHECK(e.commands.undo() && e.commands.undo()); // far, two: not this scene's
    GIANT_CHECK(e.commands.undo() && e.commands.redo()); // open_world
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.w.streaming.active(e.state.scene_generation) && e.w.streaming.stats().cells == 2);
    // That side held no cells yet, so every cell is read as it is now,
    // the new one included: nothing saved is lost or copied.
    GIANT_CHECK(count_named(e.state.scene, "one") == 1 && count_named(e.state.scene, "far") == 1 &&
                count_named(e.state.scene, "two") == 1);
    GIANT_CHECK(e.commands.redo() && e.commands.redo());
    GIANT_CHECK(count_named(e.state.scene, "two") == 1 && count_named(e.state.scene, "far") == 1);
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    GIANT_CHECK(count_named(e.state.scene, "one") == 1 && count_named(e.state.scene, "two") == 1 &&
                count_named(e.state.scene, "far") == 1 && e.w.streaming.stats().ids_changed == 0);
}

GIANT_TEST("world streaming: redo of new_scene after a script's new_scene gives back the user's scene") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entity", {{"name", "p"}}));
    GIANT_CHECK(e.run("new_scene"));
    GIANT_CHECK(e.run("create_entity", {{"name", "n"}}));
    GIANT_CHECK(e.script("new_scene"));
    GIANT_CHECK(e.script("create_entity", {{"name", "s"}}));
    GIANT_CHECK(e.commands.undo() && e.commands.undo()); // n (not this scene's), new_scene
    GIANT_CHECK(count_named(e.state.scene, "p") == 1);
    GIANT_CHECK(e.commands.redo() && e.commands.redo()); // new_scene, n
    GIANT_CHECK(count_named(e.state.scene, "n") == 1 && count_named(e.state.scene, "s") == 0);
}

namespace {
// How many entities named `name` the world's cell files hold, in all.
usize in_files(const std::filesystem::path& folder, const char* name) {
    usize n = 0;
    for (const auto& f : std::filesystem::directory_iterator(folder / "cells")) {
        if (!f.is_regular_file()) continue;
        std::string text;
        {
            std::ifstream in(f.path(), std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string stem = f.path().filename().string(); // "<x>_<z>.cell.json"
        i32 x = 0, z = 0;
        const char* end = stem.data() + stem.size();
        const auto [after_x, x_error] = std::from_chars(stem.data(), end, x);
        if (x_error != std::errc{} || after_x == end || *after_x != '_') continue;
        const auto [after_z, z_error] = std::from_chars(after_x + 1, end, z);
        if (z_error != std::errc{} || std::string_view(after_z, end) != ".cell.json") continue;
        std::vector<world::Entity> entities;
        if (world::load_cell(text, {x, z}, entities).has_value()) continue;
        for (const world::Entity& e : entities) n += e.name == name ? usize{1} : usize{0};
    }
    return n;
}
ai::ExecuteOptions unrecorded() {
    ai::ExecuteOptions options;
    options.record_history = false;
    return options;
}
} // namespace

GIANT_TEST("world streaming: an id one undo side holds is never handed out by another") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entity", {{"name", "one"}, {"position", {10, 0, 10}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    const ai::CommandResult z = e.commands.execute("create_entity", {{"name", "Z"}, {"position", {20, 0, 10}}});
    GIANT_CHECK(z.ok);
    GIANT_CHECK(e.run("new_scene") && e.commands.undo()); // the world side, with Z, kept by new_scene's step
    GIANT_CHECK(e.script("new_scene"));
    GIANT_CHECK(e.commands.undo() && e.commands.undo()); // Z (not this scene's), open_world
    GIANT_CHECK(e.commands.redo());                      // open_world: its side as opened
    e.settle(10.0, 10.0);
    const ai::CommandResult q =
        e.commands.execute("create_entity", {{"name", "Q"}, {"position", {100, 0, 10}}}, unrecorded());
    GIANT_CHECK(q.ok && q.value["id"] != z.value["id"]);
    GIANT_CHECK(e.script("save_world") && e.script("new_scene"));
    GIANT_CHECK(e.commands.redo() && e.commands.redo() && e.commands.undo()); // the world side with Z
    e.settle(10.0, 10.0);
    GIANT_CHECK(count_named(e.state.scene, "Z") == 1 && count_named(e.state.scene, "Q") == 1);
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(in_files(e.w.folder, "Q") == 1 && in_files(e.w.folder, "Z") == 1);
}

GIANT_TEST("world streaming: an entity put back away from the file it was saved to leaves that file") {
    for (const double to : {110.0, 3000.0}) {
        Editor e;
        if (e.w.renderer == nullptr) return;
        GIANT_CHECK(e.run("create_entity", {{"name", "a"}, {"position", {10, 0, 10}}}));
        GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
        GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
        e.settle(10.0, 10.0);
        const ai::CommandResult made = e.commands.execute("create_entity", {{"name", "X"}, {"position", {20, 0, 10}}});
        GIANT_CHECK(made.ok && e.run("save_world"));
        GIANT_CHECK(e.run("set_entity", {{"id", made.value["id"]}, {"position", {to, 0, 10}}}));
        GIANT_CHECK(e.run("save_world"));
        e.settle(9000.0, 10.0); // every cell unloads
        GIANT_CHECK(e.state.scene.size() == 0);
        GIANT_CHECK(e.commands.undo() && e.commands.undo()); // the move, the create: nothing loaded to change
        GIANT_CHECK(e.commands.redo());                      // the create: X back at (20, 10)
        GIANT_CHECK(e.run("save_world"));                    // before X's last cell loads
        GIANT_CHECK(in_files(e.w.folder, "X") == 1);
        GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
        e.settle(to, 10.0);
        e.settle(10.0, 10.0);
        GIANT_CHECK(count_named(e.state.scene, "X") == 1 && e.w.streaming.stats().ids_changed == 0);
    }
}

GIANT_TEST("world streaming: a loaded cell written since the stored bookkeeping is merged again") {
    for (const bool save_at_once : {false, true}) {
        Editor e;
        if (e.w.renderer == nullptr) return;
        GIANT_CHECK(e.run("create_entity", {{"name", "one"}, {"position", {10, 0, 10}}}));
        GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
        GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
        e.settle(10.0, 10.0);
        GIANT_CHECK(e.run("new_scene") && e.commands.undo()); // the world side kept, its cell loaded
        GIANT_CHECK(e.commands.execute("create_entity", {{"name", "two"}, {"position", {20, 0, 10}}}, unrecorded()).ok);
        GIANT_CHECK(e.script("save_world") && e.script("new_scene"));
        GIANT_CHECK(e.commands.redo() && e.commands.undo()); // that side, from before "two" was saved
        if (save_at_once) {
            GIANT_CHECK(e.run("save_world"));
        } else {
            e.settle(10.0, 10.0);
            GIANT_CHECK(count_named(e.state.scene, "one") == 1 && count_named(e.state.scene, "two") == 1);
        }
        GIANT_CHECK(in_files(e.w.folder, "one") == 1 && in_files(e.w.folder, "two") == 1);
    }
}

GIANT_TEST("world streaming: a save cut short keeps the bookkeeping true to the files") {
    for (const bool swap : {false, true}) {
        Editor e;
        if (e.w.renderer == nullptr) return;
        GIANT_CHECK(e.run("create_entities", {{"entities", {{{"name", "one"}, {"position", {10, 0, 10}}},
                                                            {{"name", "x"}, {"position", {20, 0, 10}}},
                                                            {{"name", "e"}, {"position", {140, 0, 10}}}}}}));
        GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
        GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
        e.settle(10.0, 10.0);
        world::EntityId x = 0, far = 0;
        for (const world::Entity& en : e.state.scene.entities()) {
            if (en.name == "x") x = en.id;
            if (en.name == "e") far = en.id;
        }
        if (swap) GIANT_CHECK(e.run("new_scene") && e.commands.undo()); // the world side kept: one and x
        // Removing the far cell's file fails: it is now a folder with something in it.
        const std::filesystem::path stuck = e.w.folder / "cells" / "2_0.cell.json";
        std::filesystem::remove(stuck);
        std::filesystem::create_directories(stuck / "keep");
        const ai::Json ids = {x, far};
        if (swap) {
            GIANT_CHECK(e.script("delete_entities", {{"ids", ids}}));
        } else {
            GIANT_CHECK(e.run("delete_entities", {{"ids", ids}}));
        }
        GIANT_CHECK(!e.run("save_world")); // cell (0, 0) is written without x, then it stops
        if (swap) {
            GIANT_CHECK(e.script("new_scene"));
            GIANT_CHECK(e.commands.redo() && e.commands.undo()); // the world side, x in it
        } else {
            GIANT_CHECK(e.commands.undo()); // the delete
        }
        GIANT_CHECK(count_named(e.state.scene, "x") == 1 && e.w.streaming.unsaved_cells(e.state.scene) >= 1);
        e.settle(5000.0, 10.0); // x's cell differs from its file: it stays
        e.settle(10.0, 10.0);
        GIANT_CHECK(count_named(e.state.scene, "x") == 1);
        std::filesystem::remove_all(stuck);
    }
}

GIANT_TEST("world streaming: a cell an entity left, emptied by a save mid-read, stays gone") {
    for (const bool missing : {false, true}) {
        Editor e;
        if (e.w.renderer == nullptr) return;
        GIANT_CHECK(e.run("create_entity", {{"name", "a"}, {"position", {10, 0, 10}}}));
        GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
        GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}}));
        e.settle(10.0, 10.0);
        const ai::CommandResult made = e.commands.execute("create_entity", {{"name", "X"}, {"position", {20, 0, 10}}});
        GIANT_CHECK(made.ok && e.run("save_world"));
        GIANT_CHECK(e.run("set_entity", {{"id", made.value["id"]}, {"position", {110, 0, 10}}})); // cell (1, 0)
        GIANT_CHECK(e.run("save_world"));
        e.settle(9000.0, 10.0);
        GIANT_CHECK(e.commands.undo() && e.commands.undo() && e.commands.redo()); // X back in (0, 0)
        if (missing) {
            // Its old cell's file removed by hand: taken as empty, not a reason to refuse.
            std::filesystem::remove(e.w.folder / "cells" / "1_0.cell.json");
        } else {
            // One frame starts reading both cells; the save finds (1, 0) parsed.
            scene_frame(e.state.scene, *e.w.renderer, *e.w.meshes, e.w.frame);
            e.w.streaming.update(e.state.scene, e.state.scene_generation, {110.0, 10.0, 10.0}, e.w.frame);
        }
        const ai::CommandResult saved = e.commands.execute("save_world");
        GIANT_CHECK(saved.ok);
        GIANT_CHECK(!std::filesystem::exists(e.w.folder / "cells" / "1_0.cell.json"));
        e.settle(110.0, 10.0);
        GIANT_CHECK(count_named(e.state.scene, "X") == 1 && e.w.streaming.stats().ids_changed == 0);
        GIANT_CHECK(e.w.streaming.stats().cells == 1 && e.w.streaming.stats().loaded == 1);
        GIANT_CHECK(e.run("save_world") && in_files(e.w.folder, "X") == 1);
    }
}

GIANT_TEST("world streaming: ids seen in one world do not carry into the next") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    ai::Json many = ai::Json::array();
    for (int i = 0; i < 40; ++i) many.push_back({{"position", {10.0 + i, 0.0, 10.0}}});
    GIANT_CHECK(e.run("create_entities", {{"entities", many}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world"));
    World other; // a second world folder, one entity
    other.add(10.0, 10.0, "b");
    GIANT_CHECK(other.streaming.create(other.folder, other.scene, other.generation, {}).error.empty());
    other.streaming.close();
    GIANT_CHECK(e.run("open_world", {{"folder", other.folder.generic_string()}}));
    GIANT_CHECK(e.state.scene.next_id() == 2);
}

namespace {
world::Terrain::Heights ramp(u32 samples, float base) {
    world::Terrain::Heights h(usize{samples} * samples);
    for (usize k = 0; k < h.size(); ++k) h[k] = base + 0.25f * static_cast<float>(k % 7) - 0.1f * static_cast<float>(k / 97);
    return h;
}
} // namespace

GIANT_TEST("world streaming: terrain tiles are saved in their cell's file and stream with it") {
    World w;
    if (w.renderer == nullptr) return;
    w.scene.terrain.set_samples(16);
    w.scene.terrain.material.roughness = 0.5f;
    w.scene.terrain.set({0, 0}, ramp(16, 1.0f));
    w.scene.terrain.set({1, 0}, ramp(16, 2.0f));
    w.scene.terrain.set({40, 0}, ramp(16, 3.0f));
    w.add(100.0, 10.0, "next"); // cell (1, 0): entities and a tile in one file
    const world::Terrain made = w.scene.terrain;
    const auto saved = w.streaming.create(w.folder, w.scene, w.generation, {});
    GIANT_CHECK(saved.error.empty() && saved.cells_written == 3 && saved.cells == 3);
    GIANT_CHECK(w.streaming.stats().ground == 3 && w.streaming.ground_samples() == 16u);

    w.reopen();
    GIANT_CHECK(w.scene.terrain.empty() && w.scene.terrain.samples() == 16);
    GIANT_CHECK(w.scene.terrain.material.roughness == 0.5f);
    GIANT_CHECK(w.streaming.index().terrain_cells.size() == 3);
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.terrain.size() == 2 && w.named("next") == 1);
    GIANT_CHECK(*w.scene.terrain.find({0, 0}) == *made.find({0, 0})); // bit for bit
    GIANT_CHECK(*w.scene.terrain.find({1, 0}) == *made.find({1, 0}));
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 0 && w.streaming.stats().tiles_loaded == 2);

    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.scene.terrain.size() == 1 && *w.scene.terrain.find({40, 0}) == *made.find({40, 0}));
    GIANT_CHECK(w.scene.size() == 0 && w.streaming.stats().tiles_unloaded == 2);
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 0);
    // Nothing changed: a save writes no cell.
    const auto again = w.streaming.save(w.scene, {});
    GIANT_CHECK(again.error.empty() && again.cells_written == 0 && again.cells_removed == 0);
}

GIANT_TEST("world streaming: an edited tile stays loaded until saved; a cell left without ground loses its file") {
    World w;
    if (w.renderer == nullptr) return;
    w.scene.terrain.set_samples(8);
    w.scene.terrain.set({0, 0}, ramp(8, 1.0f));
    w.scene.terrain.set({1, 0}, ramp(8, 2.0f));
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.terrain.size() == 2);
    (*w.scene.terrain.modify({0, 0}))[5] = 9.5f;
    GIANT_CHECK(w.scene.terrain.remove({1, 0}).has_value());
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 2);
    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.scene.terrain.size() == 1); // the edited tile stays; nothing to unload in (1, 0)
    const auto saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.cells_written == 1 && saved.cells_removed == 1 && saved.cells == 1);
    GIANT_CHECK(!std::filesystem::exists(w.folder / "cells" / "1_0.cell.json"));
    GIANT_CHECK(w.streaming.stats().ground == 1 && w.streaming.index().terrain_cells.size() == 1);
    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.scene.terrain.empty()); // saved: it goes
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.terrain.size() == 1 && w.scene.terrain.find({0, 0})->at(5) == 9.5f);
}

GIANT_TEST("world streaming: a tile made in a cell still on disk keeps the file's entities, and wins over its tile") {
    World w;
    if (w.renderer == nullptr) return;
    w.scene.terrain.set_samples(8);
    w.add(2570.0, 10.0, "far");            // cell (40, 0)
    w.scene.terrain.set({41, 0}, ramp(8, 1.0f)); // a tile on disk
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.size() == 0 && w.scene.terrain.empty());
    // Saved straight away, before streaming reads the cells: the files are merged.
    w.scene.terrain.set({40, 0}, ramp(8, 5.0f));
    w.scene.terrain.set({41, 0}, ramp(8, 6.0f));
    const auto saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.cells_written == 2);
    w.reopen();
    w.settle(2600.0, 10.0);
    GIANT_CHECK(w.named("far") == 1 && w.scene.terrain.size() == 2);
    GIANT_CHECK(*w.scene.terrain.find({40, 0}) == ramp(8, 5.0f) && *w.scene.terrain.find({41, 0}) == ramp(8, 6.0f));

    // Not saved: the cells are read anyway (whatever the distance), and the scene's tile stays.
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.terrain.empty());
    w.scene.terrain.set({41, 0}, ramp(8, 7.0f));
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.streaming.stats().loaded == 1 && *w.scene.terrain.find({41, 0}) == ramp(8, 7.0f));
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 1);
}

GIANT_TEST("world streaming: the terrain keeps the spacing of the world's tiles") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_terrain", {{"from", {0, 0}}, {"to", {10, 10}}, {"samples", 16}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.w.streaming.ground_samples() == 16u);
    // Undo takes the tile away and the spacing back to 64; streaming puts the world's back.
    GIANT_CHECK(e.commands.undo());
    GIANT_CHECK(e.state.scene.terrain.empty() && e.state.scene.terrain.samples() == 64);
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.terrain.samples() == 16);
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 1); // the tile's cell, emptied
    GIANT_CHECK(e.commands.redo() && e.state.scene.terrain.size() == 1);
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 0);
    // New ground takes the world's spacing; another is refused.
    const ai::CommandResult refused =
        e.commands.execute("create_terrain", {{"from", {200, 0}}, {"to", {210, 10}}, {"samples", 64}});
    GIANT_CHECK(!refused.ok && refused.error.find("16 samples") != std::string::npos);
    const ai::CommandResult made = e.commands.execute("create_terrain", {{"from", {200, 0}}, {"to", {210, 10}}});
    GIANT_CHECK(made.ok && made.value["spacing"] == 4.0);
    // A file whose tile is another spacing fails alone.
    write_file(e.w.folder / "cells" / "9_0.cell.json", R"({"format": "giant-cell", "version": 1, "cell": [9, 0],
        "entities": [], "heights": ["0 0 0 0 0 0 0 0", "0 0 0 0 0 0 0 0", "0 0 0 0 0 0 0 0", "0 0 0 0 0 0 0 0",
                                    "0 0 0 0 0 0 0 0", "0 0 0 0 0 0 0 0", "0 0 0 0 0 0 0 0", "0 0 0 0 0 0 0 0"]})");
    GIANT_CHECK(e.run("save_world"));
    world::WorldIndex index = e.w.streaming.index();
    index.cells.push_back({9, 0});
    write_file(e.w.folder / "world.json", world::save_world_index(index));
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}}));
    e.settle(600.0, 10.0);
    GIANT_CHECK(e.w.streaming.stats().failed == 1 && e.state.scene.terrain.find({9, 0}) == nullptr);
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.terrain.size() == 2);
}

GIANT_TEST("world streaming: undo and redo of terrain edits in a streamed world") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_terrain", {{"from", {0, 0}}, {"to", {100, 10}}, {"samples", 8}, {"height", 2}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("set_terrain_heights", {{"cell", {0, 0}}, {"heights", ai::Json(std::vector<float>(64, 4.0f))}}));
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 1);
    GIANT_CHECK(e.commands.undo() && e.w.streaming.unsaved_cells(e.state.scene) == 0);
    e.settle(2570.0, 10.0);
    GIANT_CHECK(e.state.scene.terrain.empty());
    // Redo puts the edit back in a cell now on disk: read again, the scene's tile wins.
    GIANT_CHECK(e.commands.redo() && e.state.scene.terrain.size() == 1);
    e.settle(2570.0, 10.0);
    GIANT_CHECK(e.state.scene.terrain.size() == 1 && e.state.scene.terrain.find({0, 0})->at(0) == 4.0f);
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 1);
    GIANT_CHECK(e.run("save_world"));
    e.settle(2570.0, 10.0);
    GIANT_CHECK(e.state.scene.terrain.empty());
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.terrain.size() == 2 && e.state.scene.terrain.find({0, 0})->at(0) == 4.0f &&
                e.state.scene.terrain.find({1, 0})->at(0) == 2.0f);
}

GIANT_TEST("world streaming: terrain commands leave ground on disk alone until it is loaded") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_terrain", {{"from", {2560, 0}}, {"to", {2570, 10}}, {"samples", 8}, {"height", 3}}));
    GIANT_CHECK(e.run("create_entities", {{"entities", {{{"position", {2600, 0, 20}}}}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0); // cell (40, 0) stays on disk
    GIANT_CHECK(e.w.streaming.ground_on_disk({40, 0}) && e.state.scene.terrain.empty());
    // Made, set or removed there, the undo would not know the file's tile: refused.
    ai::CommandResult r = e.commands.execute("create_terrain", {{"from", {2560, 0}}, {"to", {2570, 10}}, {"height", 9}});
    GIANT_CHECK(!r.ok && r.error.find("not loaded") != std::string::npos);
    r = e.commands.execute("set_terrain_heights", {{"cell", {40, 0}}, {"heights", ai::Json(std::vector<float>(64, 9.0f))}});
    GIANT_CHECK(!r.ok && e.state.scene.terrain.empty());
    r = e.commands.execute("remove_terrain", {{"from", {2560, 0}}, {"to", {2570, 10}}});
    GIANT_CHECK(!r.ok && r.error.find("not loaded") != std::string::npos);
    // Next to it is fine; loaded, it is too, and undo then save keeps the file's tile.
    GIANT_CHECK(e.run("create_terrain", {{"from", {2500, 0}}, {"to", {2510, 10}}, {"height", 1}}));
    e.settle(2565.0, 5.0);
    GIANT_CHECK(!e.w.streaming.ground_on_disk({40, 0}) && e.state.scene.terrain.find({40, 0})->at(0) == 3.0f);
    GIANT_CHECK(e.run("set_terrain_heights", {{"cell", {40, 0}}, {"heights", ai::Json(std::vector<float>(64, 9.0f))}}));
    GIANT_CHECK(e.commands.undo());
    const ai::CommandResult saved = e.commands.execute("save_world");
    GIANT_CHECK(saved.ok && saved.value["cells_removed"] == 0);
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}}));
    e.settle(2565.0, 5.0);
    GIANT_CHECK(e.state.scene.terrain.find({40, 0}) != nullptr && e.state.scene.terrain.find({40, 0})->at(0) == 3.0f);
    GIANT_CHECK(e.state.scene.size() == 1);
}

GIANT_TEST("world streaming: undo puts back tiles of a spacing the world has since left") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_terrain", {{"from", {0, 0}}, {"to", {10, 10}}, {"samples", 16}, {"height", 1}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("remove_terrain", {{"all", true}}) && e.run("save_world"));
    GIANT_CHECK(!e.w.streaming.ground_samples());
    GIANT_CHECK(e.run("create_terrain", {{"from", {200, 0}}, {"to", {210, 10}}, {"samples", 32}}));
    GIANT_CHECK(e.run("save_world") && e.w.streaming.ground_samples() == 32u);
    GIANT_CHECK(e.commands.undo()); // the 32-sample tile goes; spacing 16
    e.settle(10.0, 10.0);             // streaming gives the empty terrain the world's 32
    GIANT_CHECK(e.state.scene.terrain.samples() == 32);
    GIANT_CHECK(e.commands.undo()); // the 16-sample tile comes back, its spacing with it (no assert)
    GIANT_CHECK(e.state.scene.terrain.size() == 1 && e.state.scene.terrain.samples() == 16);
    GIANT_CHECK(e.state.scene.terrain.find({0, 0})->at(0) == 1.0f);
    // Saved: the cell left with no ground loses its file, the 16-sample tile gets one.
    const ai::CommandResult saved = e.commands.execute("save_world");
    GIANT_CHECK(saved.ok && saved.value["cells_removed"] == 1);
    GIANT_CHECK(e.w.streaming.ground_samples() == 16u);
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}}));
    e.settle(10.0, 10.0);
    GIANT_CHECK(e.state.scene.terrain.size() == 1 && e.state.scene.terrain.samples() == 16);
}

GIANT_TEST("world streaming: tiles join within the per-frame budget and leave a few cells a frame") {
    World w;
    if (w.renderer == nullptr) return;
    w.scene.terrain.set_samples(16); // a tile weighs 4 entities
    for (i32 x = 0; x < 3; ++x) {
        for (i32 z = 0; z < 4; ++z) w.scene.terrain.set({x, z}, ramp(16, static_cast<float>(x + z)));
    }
    GIANT_CHECK(w.streaming.create(w.folder, w.scene, w.generation, {}).error.empty());
    w.reopen();
    w.streaming.settings.entities_per_frame = 8;
    w.step({90.0, 10.0, 120.0}); // starts reading all twelve
    w.streaming.finish_reads();
    w.step({90.0, 10.0, 120.0});
    GIANT_CHECK(w.scene.terrain.size() == 2); // 8 entities' worth
    w.settle(90.0, 120.0);
    GIANT_CHECK(w.scene.terrain.size() == 12 && w.streaming.unsaved_cells(w.scene) == 0);
    // Far off: three cells a frame go.
    w.streaming.settings.unloads_per_frame = 3;
    w.step({5000.0, 10.0, 0.0});
    GIANT_CHECK(w.scene.terrain.size() == 9);
    w.step({5000.0, 10.0, 0.0});
    GIANT_CHECK(w.scene.terrain.size() == 6);
    w.settle(5000.0, 0.0);
    GIANT_CHECK(w.scene.terrain.empty() && w.streaming.stats().tiles_unloaded == 12);
}

GIANT_TEST("world streaming: a tile undone to what its file holds is clean again, with no copy kept") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_terrain", {{"from", {0, 0}}, {"to", {10, 10}}, {"samples", 8}, {"height", 2}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 0);
    GIANT_CHECK(e.run("set_terrain_heights", {{"cell", {0, 0}}, {"heights", ai::Json(std::vector<float>(64, 5.0f))}}));
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 1);
    GIANT_CHECK(e.commands.undo() && e.w.streaming.unsaved_cells(e.state.scene) == 0);
    GIANT_CHECK(e.commands.redo() && e.w.streaming.unsaved_cells(e.state.scene) == 1);
    // Set back to the same heights by hand is a change (versions, not a compare): saved, clean.
    GIANT_CHECK(e.run("set_terrain_heights", {{"cell", {0, 0}}, {"heights", ai::Json(std::vector<float>(64, 2.0f))}}));
    GIANT_CHECK(e.w.streaming.unsaved_cells(e.state.scene) == 1);
    const ai::CommandResult saved = e.commands.execute("save_world");
    GIANT_CHECK(saved.ok && saved.value["cells_written"] == 1 && e.w.streaming.unsaved_cells(e.state.scene) == 0);
}

GIANT_TEST("world streaming: the weather is saved in world.json and comes back on open") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    world::Weather storm;
    storm.cloud_cover = 0.9f;
    storm.fog_density = 0.25f;
    e.state.scene.weather = storm;
    GIANT_CHECK(e.run("create_entities", {{"entities", {{{"position", {5, 0, 5}}}}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("close_world") && e.run("new_scene"));
    GIANT_CHECK(!e.state.scene.weather.has_value());
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.state.scene.weather == storm);
    // A change saved again with the world open, and cleared: written as none.
    e.state.scene.weather->haze = 3.0f;
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}, {"discard_unsaved", true}}));
    GIANT_CHECK(e.state.scene.weather && e.state.scene.weather->haze == 3.0f);
    e.state.scene.weather.reset();
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(e.run("close_world") && e.run("open_world", {{"folder", e.folder()}, {"discard_unsaved", true}}));
    GIANT_CHECK(!e.state.scene.weather.has_value());
}

GIANT_TEST("world streaming: unsaved weather, lighting, kits or scatter keep open_world from discarding them") {
    Editor e;
    if (e.w.renderer == nullptr) return;
    GIANT_CHECK(e.run("create_entities", {{"entities", {{{"position", {5, 0, 5}}}}}}));
    GIANT_CHECK(e.run("save_world", {{"folder", e.folder()}}));
    GIANT_CHECK(!e.commands.execute("get_world").value["unsaved_settings"].get<bool>());
    e.state.scene.weather = world::Weather{};
    GIANT_CHECK(e.commands.execute("get_world").value["unsaved_settings"].get<bool>());
    GIANT_CHECK(!e.run("open_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("save_world"));
    GIANT_CHECK(!e.commands.execute("get_world").value["unsaved_settings"].get<bool>());
    e.state.scene.lighting.exposure = 2.0f;
    GIANT_CHECK(!e.run("open_world", {{"folder", e.folder()}}));
    GIANT_CHECK(e.run("open_world", {{"folder", e.folder()}, {"discard_unsaved", true}}));
    GIANT_CHECK(e.state.scene.weather.has_value() && e.state.scene.lighting.exposure == 1.0f);
}

GIANT_TEST("world streaming: kept damage is saved in its cell's file, streams with it, and holds a cell as unsaved") {
    World w;
    if (w.renderer == nullptr) return;
    w.add(10.0, 10.0, "near");
    world::CellDamage near;
    world::KeptTree t;
    t.x = 20.0;
    t.z = 20.0;
    t.position = {20.0, 0.5, 20.0};
    near.trees.push_back(t);
    w.scene.set_damage({0, 0}, near);
    world::CellDamage far = near; // a cell with nothing but damage
    far.trees[0].x = t.position.x = 2580.0;
    far.trees[0].position = {2580.0, 0.5, 20.0};
    w.scene.set_damage({40, 0}, far);
    auto saved = w.streaming.create(w.folder, w.scene, w.generation, {});
    GIANT_CHECK(saved.error.empty() && saved.cells == 2);
    GIANT_CHECK(std::filesystem::exists(w.folder / "cells" / "40_0.cell.json"));
    w.reopen();
    GIANT_CHECK(w.scene.damage().empty());
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.damage_in({0, 0}) != nullptr && *w.scene.damage_in({0, 0}) == near);
    GIANT_CHECK(w.scene.damage_in({40, 0}) == nullptr && w.streaming.unsaved_cells(w.scene) == 0);
    // Far off: it unloads with its cell, and the far cell's comes.
    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.scene.damage_in({0, 0}) == nullptr && w.scene.damage_in({40, 0}) != nullptr);
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 0);
    // Changed: unsaved, and the cell stays loaded however far the camera goes.
    world::CellDamage more = far;
    more.trees[0].position.y = 1.0;
    w.scene.set_damage({40, 0}, more);
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 1);
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.damage_in({40, 0}) != nullptr && *w.scene.damage_in({40, 0}) == more);
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.cells_written == 1 && w.streaming.unsaved_cells(w.scene) == 0);
    w.settle(10.0, 10.0);
    GIANT_CHECK(w.scene.damage_in({40, 0}) == nullptr); // saved: it may go now
    w.settle(2570.0, 10.0);
    GIANT_CHECK(w.scene.damage_in({40, 0}) != nullptr && *w.scene.damage_in({40, 0}) == more);
    // Taken away, the file of a cell with nothing else goes on saving.
    w.scene.set_damage({40, 0}, {});
    GIANT_CHECK(w.streaming.unsaved_cells(w.scene) == 1);
    saved = w.streaming.save(w.scene, {});
    GIANT_CHECK(saved.error.empty() && saved.cells_removed == 1);
    GIANT_CHECK(!std::filesystem::exists(w.folder / "cells" / "40_0.cell.json"));
}
