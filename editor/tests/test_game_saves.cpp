// Game saves in the editor: what simulating broke saved beside the world
// (never in its files), shown again on loading over a whole world, with
// the camera and the weather; only changes written again; another world's
// save refused unless forced; slots listed and deleted; the game closed.
#include "../src/editor_commands.h"
#include "../src/game_saves.h"
#include "../src/physics_state.h"
#include "../src/play_state.h"
#include "../src/scatter_placer.h"
#include "../src/weather_state.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/world/scene.h"
#include "giant/world/weather.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

std::filesystem::path fresh_folder() {
    static std::atomic<int> n{0};
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("giant_game_save_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
         std::to_string(n++));
    std::filesystem::remove_all(p);
    return p;
}

std::string read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The world's own files (not its saves), by path.
std::map<std::string, std::string> world_files(const std::filesystem::path& folder) {
    std::map<std::string, std::string> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(folder)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = std::filesystem::relative(e.path(), folder).generic_string();
        if (rel.rfind("saves/", 0) == 0) continue;
        out[rel] = read_all(e.path());
    }
    return out;
}

} // namespace

GIANT_TEST("game saves: what simulating broke is saved beside the world and shown again on loading") {
    const std::filesystem::path folder = fresh_folder();
    {
        PhysicsState physics;
        WeatherState weather;
        WorldStreaming streaming;
        GameSaves saves;
        EditorState state;
        state.physics = &physics;
        state.weather = &weather;
        state.world = &streaming;
        state.saves = &saves;
        ai::CommandRegistry commands;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_save_commands(commands, state);
        const auto run = [&](const char* name, const Json& args = Json::object()) {
            return commands.execute(name, args, {});
        };
        world::Scene& scene = state.scene;
        scene.terrain.set_samples(16);
        for (i32 x = 0; x <= 3; ++x) scene.terrain.set({x, 0}, world::Terrain::Heights(256, 0.0f));
        world::ScatterLayer l;
        l.name = "pines";
        l.spacing = 16.0f;
        l.jitter = 0.0f;
        world::ScatterItem item;
        item.shape = world::Shape::cube;
        item.tree = world::ScatterTree{};
        l.items = {item};
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
        scene.add(e);
        GIANT_CHECK(!run("save_game", {{"slot", "one"}}).ok); // no world open
        // The world keeps a fallen tree in cell 3 (the forest knows it; play never touches it).
        world::CellDamage kept3;
        world::KeptTree t3;
        t3.x = 250.0;
        t3.z = 10.0;
        t3.position = {250.0, 0.0, 10.0};
        kept3.trees.push_back(t3);
        scene.set_damage({3, 0}, kept3);
        GIANT_CHECK(streaming.create(folder, scene, state.scene_generation, {}).error.empty());
        const std::map<std::string, std::string> files = world_files(folder);
        ScatterPlacer placer;
        placer.finish(scene, scene.terrain.take_changes());
        const DVec3 far{3000.0, 10.0, 32.0}; // rubble and laid trees, no bodies
        const auto frame = [&] {
            physics.feed_forest(scene, placer, far);
            physics.update(scene, scene.take_changes(), 1.0 / 60.0);
            saves.update(state, 1.0 / 60.0); // (before the weather, as the main loop has it)
            weather.update(saves.weather() != nullptr ? *saves.weather() : scene.weather, 1.0 / 60.0, far,
                           state.scene_generation);
        };
        scene.weather = world::weather_preset("rain");
        frame();
        // Broken and felled while simulating.
        GIANT_CHECK(run("simulate", {{"on", true}}).ok);
        ai::CommandResult r = run("impact", {{"point", {32.6, 1.5, 32.0}}, {"radius", 0.3}, {"impulse", 6000.0}});
        GIANT_CHECK(r.ok && r.value["loosened"] == 2);
        r = run("impact", {{"point", {160.0, 1.0, 32.0}}, {"radius", 20.0}, {"impulse", 1e6}});
        const u64 felled = r.value["felled"].get<u64>();
        GIANT_CHECK(felled > 2);
        frame();
        state.camera.position = {40.0, 3.0, 20.0};
        state.camera.yaw_degrees = 33.0f;
        r = run("save_game", {{"slot", "one"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["saves"] == 1 && r.value["error"] == "" && r.value["base_slot"] == "one");
        const u64 cells = r.value["cells"].get<u64>();
        GIANT_CHECK(cells >= 2 && r.value["written"] == cells + 3); // the cells and two sections
        // The world itself is untouched.
        GIANT_CHECK(scene.damage().size() == 1 && scene.game_damage().empty() && world_files(folder) == files);
        GIANT_CHECK(std::filesystem::exists(folder / "saves" / "one" / "save.json"));

        // Stopped: whole again; elsewhere, other weather.
        GIANT_CHECK(run("simulate", {{"on", false}}).ok);
        scene.weather = world::weather_preset("clear");
        frame();
        r = run("get_physics");
        GIANT_CHECK(r.value["broken_kits"] == 0 && r.value["trees"]["fallen"] == 0);
        state.camera.position = {0.0, 50.0, 0.0};
        // Loaded: the save's damage shows as the game's, the camera and the weather come back.
        r = run("load_game", {{"slot", "one"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["loads"] == 1 && r.value["game_cells"] == cells && r.value["weather_override"] == true);
        GIANT_CHECK(state.camera.position == DVec3(40.0, 3.0, 20.0) && state.camera.yaw_degrees == 33.0f);
        GIANT_CHECK(saves.weather() != nullptr && *saves.weather() == *world::weather_preset("rain"));
        for (int i = 0; i < 3; ++i) frame();
        physics.flush(scene);
        r = run("get_physics");
        GIANT_CHECK(r.value["kept"]["pieces_down"] == 2 && r.value["kept"]["trees_down"] == felled);
        GIANT_CHECK(r.value["kept"]["cells"] == 1 && r.value["kept"]["game"]["cells"] == cells);
        GIANT_CHECK(weather.shown() && *weather.shown() == *world::weather_preset("rain"));
        GIANT_CHECK(!run("keep_damage").ok); // the game's is not the world's
        // Saved again, nothing changed: only the sections are written.
        r = run("save_game", {{"slot", "one"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["written"] == 3 && r.value["cells"] == cells);
        GIANT_CHECK(world_files(folder) == files);
        GIANT_CHECK(scene.game_damage_in({3, 0}) == nullptr); // the world's alone there: not the save's
        // The scene's weather changed: it shows again.
        scene.weather = world::weather_preset("fog");
        frame();
        GIANT_CHECK(saves.weather() == nullptr && weather.shown() && *weather.shown() == *world::weather_preset("fog"));
        // A base slot deleted on the disk: the next save there is whole, not refused.
        GIANT_CHECK(run("delete_save", {{"slot", "one"}, {"wait", true}}).ok);
        GIANT_CHECK(run("get_save").value["base_slot"] == "");
        r = run("save_game", {{"slot", "one"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["written"] == cells + 3);

        // Another slot (whole), then made to look of another world: refused unless forced.
        r = run("save_game", {{"slot", "two"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["written"] == cells + 3);
        const std::filesystem::path manifest = folder / "saves" / "two" / "save.json";
        std::string text = read_all(manifest);
        const usize at = text.find("\"hash\": \"");
        GIANT_CHECK(at != std::string::npos);
        text[at + 9] = text[at + 9] == '0' ? '1' : '0';
        {
            std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
            out << text;
        }
        r = run("list_saves");
        GIANT_CHECK(r.ok && r.value["slots"].size() == 2 && r.value["slots"][0]["of_this_world"] == true &&
                    r.value["slots"][1]["of_this_world"] == false);
        r = run("load_game", {{"slot", "two"}, {"wait", true}});
        GIANT_CHECK(!r.ok && r.error.find("force") != std::string::npos);
        GIANT_CHECK(scene.game_damage().size() == cells); // left as it was
        r = run("load_game", {{"slot", "two"}, {"force", true}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["game_cells"] == cells);
        GIANT_CHECK(!run("load_game", {{"slot", "none"}, {"wait", true}}).ok);
        GIANT_CHECK(!run("load_game", {{"slot", "Bad"}}).ok);
        // Deleted; closed: the world shows as it is kept (whole).
        GIANT_CHECK(run("delete_save", {{"slot", "two"}, {"wait", true}}).ok && !run("delete_save", {{"slot", "two"}}).ok);
        GIANT_CHECK(run("list_saves").value["slots"].size() == 1);
        r = run("close_game");
        GIANT_CHECK(r.ok && r.value["game_cells"] == 0 && r.value["base_slot"] == "");
        for (int i = 0; i < 3; ++i) frame();
        r = run("get_physics");
        GIANT_CHECK(r.value["kept"]["pieces_down"] == 0 && r.value["kept"]["trees_down"] == 0);
        // Not waited for: started, and done by the frames.
        r = run("load_game", {{"slot", "one"}});
        GIANT_CHECK(r.ok && r.value["doing"] == "loading");
        GIANT_CHECK(!run("save_game", {{"slot", "one"}}).ok); // one at a time
        for (int i = 0; i < 200 && saves.busy(); ++i) {
            saves.wait();
            frame();
        }
        GIANT_CHECK(!saves.busy() && scene.game_damage().size() == cells && saves.status().loads == 3);
        // A new scene: the game is not its.
        GIANT_CHECK(run("new_scene").ok);
        frame();
        GIANT_CHECK(saves.status().base_slot.empty() && scene.game_damage().empty());
        GIANT_CHECK(!run("save_game", {{"slot", "one"}}).ok);
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

GIANT_TEST("game saves: a cell's kept trees stay with a new break there; set_weather ends a game's weather") {
    const std::filesystem::path folder = fresh_folder();
    {
        PhysicsState physics;
        WeatherState weather;
        WorldStreaming streaming;
        GameSaves saves;
        EditorState state;
        state.physics = &physics;
        state.weather = &weather;
        state.world = &streaming;
        state.saves = &saves;
        ai::CommandRegistry commands;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_weather_commands(commands, state);
        register_save_commands(commands, state);
        const auto run = [&](const char* name, const Json& args = Json::object()) {
            return commands.execute(name, args, {});
        };
        world::Scene& scene = state.scene;
        scene.terrain.set_samples(16);
        scene.terrain.set({0, 0}, world::Terrain::Heights(256, 0.0f));
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
        scene.add(e);
        // A kept tree record in the tower's cell (the world's; no forest knows the cell: no scatter).
        world::CellDamage kept;
        world::KeptTree t;
        t.x = 10.0;
        t.z = 10.0;
        t.position = {10.0, 0.0, 10.0};
        kept.trees.push_back(t);
        scene.set_damage({0, 0}, kept);
        scene.weather = world::weather_preset("clear");
        GIANT_CHECK(streaming.create(folder, scene, state.scene_generation, {}).error.empty());
        ScatterPlacer placer;
        placer.finish(scene, scene.terrain.take_changes());
        const DVec3 far{3000.0, 10.0, 32.0};
        const auto frame = [&] {
            physics.feed_forest(scene, placer, far);
            physics.update(scene, scene.take_changes(), 1.0 / 60.0);
            saves.update(state, 1.0 / 60.0);
            weather.update(saves.weather() != nullptr ? *saves.weather() : scene.weather, 1.0 / 60.0, far,
                           state.scene_generation);
        };
        frame();
        GIANT_CHECK(run("simulate", {{"on", true}}).ok);
        GIANT_CHECK(run("impact", {{"point", {32.6, 1.5, 32.0}}, {"radius", 0.3}, {"impulse", 6000.0}}).value["loosened"] == 2);
        frame();
        GIANT_CHECK(run("save_game", {{"slot", "mixed"}, {"wait", true}}).ok);
        GIANT_CHECK(run("simulate", {{"on", false}}).ok);
        scene.weather = world::weather_preset("storm"); // (a value the save's is not)
        frame();
        GIANT_CHECK(run("load_game", {{"slot", "mixed"}, {"wait", true}}).ok);
        // The game's cell holds the break and the world's tree both.
        const world::CellDamage* shown = scene.shown_damage_in({0, 0});
        GIANT_CHECK(shown != nullptr && scene.game_damage_in({0, 0}) == shown);
        GIANT_CHECK(shown->kits.size() == 1 && shown->kits[0].pieces.size() == 2 && shown->trees.size() == 1);
        // The save's weather shows; set_weather (to the scene's own, even) ends it.
        frame();
        GIANT_CHECK(saves.weather() != nullptr && *saves.weather() == *world::weather_preset("clear"));
        GIANT_CHECK(run("set_weather", {{"preset", "storm"}}).ok && saves.weather() == nullptr);
        frame();
        GIANT_CHECK(weather.shown() && *weather.shown() == *world::weather_preset("storm"));
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

GIANT_TEST("game saves: the ground dented is saved and pressed in again, never into the world's files") {
    const std::filesystem::path folder = fresh_folder();
    {
        PhysicsState physics;
        WorldStreaming streaming;
        GameSaves saves;
        EditorState state;
        state.physics = &physics;
        state.world = &streaming;
        state.saves = &saves;
        ai::CommandRegistry commands;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_save_commands(commands, state);
        const auto run = [&](const char* name, const Json& args = Json::object()) {
            return commands.execute(name, args, {});
        };
        world::Scene& scene = state.scene;
        scene.terrain.set_samples(16);
        for (i32 x = 0; x <= 2; ++x) scene.terrain.set({x, 0}, world::Terrain::Heights(256, 1.0f));
        GIANT_CHECK(streaming.create(folder, scene, state.scene_generation, {}).error.empty());
        const std::map<std::string, std::string> files = world_files(folder);
        const world::Terrain::Heights authored = *scene.terrain.find({0, 0});
        const u64 v0 = scene.terrain.version({0, 0});
        const auto frame = [&] {
            physics.update(scene, scene.take_changes(), 1.0 / 60.0);
            saves.update(state, 1.0 / 60.0);
        };
        frame();
        // A crater over the edge of tiles 0 and 1 while simulating.
        GIANT_CHECK(run("simulate", {{"on", true}}).ok);
        world::Dent crater;
        crater.radius = 6.0;
        crater.depth = 2.0f;
        GIANT_CHECK(physics.dent(scene, crater, {64.0, 1.0, 32.0}) == 2);
        const world::Terrain::Heights dented0 = *scene.terrain.find({0, 0});
        const world::Terrain::Heights dented1 = *scene.terrain.find({1, 0});
        GIANT_CHECK(dented0 != authored);
        ai::CommandResult r = run("save_game", {{"slot", "dug"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["tiles"] == 2 && r.value["cells"] == 2);
        GIANT_CHECK(world_files(folder) == files);
        // Stopped: the ground as authored again.
        GIANT_CHECK(run("simulate", {{"on", false}}).ok);
        frame();
        GIANT_CHECK(*scene.terrain.find({0, 0}) == authored && scene.terrain.version({0, 0}) == v0);
        // Loaded: the save's ground pressed in, the world's files never written with it.
        saves.ground_tiles_per_frame = 1;
        r = run("load_game", {{"slot", "dug"}});
        GIANT_CHECK(r.ok);
        saves.wait();
        frame();
        GIANT_CHECK(physics.game_ground().tiles() == 1 && saves.busy()); // a tile a frame
        frame();
        frame();
        GIANT_CHECK(!saves.busy() && physics.game_ground().tiles() == 2);
        GIANT_CHECK(*scene.terrain.find({0, 0}) == dented0 && *scene.terrain.find({1, 0}) == dented1);
        GIANT_CHECK(!run("keep_damage").ok && !run("clear_damage").ok); // a game's ground is shown
        r = run("save_world");
        GIANT_CHECK(r.ok);
        GIANT_CHECK(world_files(folder) == files);
        GIANT_CHECK(*scene.terrain.find({0, 0}) == dented0); // (still shown)
        // Simulating over it: stopping goes back to the game's ground, not the world's.
        GIANT_CHECK(run("simulate", {{"on", true}}).ok);
        GIANT_CHECK(physics.dent(scene, crater, {20.0, 1.0, 20.0}) == 1);
        GIANT_CHECK(*scene.terrain.find({0, 0}) != dented0);
        r = run("save_game", {{"slot", "dug"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["tiles"] == 2 && r.value["written"] == 4); // tile 0's cell, camera, weather, bodies
        GIANT_CHECK(run("simulate", {{"on", false}}).ok);
        frame();
        GIANT_CHECK(*scene.terrain.find({0, 0}) == dented0);
        // Sculpted over: that tile's game ground goes (the edit is the world's now), the other stays.
        scene.terrain.modify({1, 0})->at(0) = 5.0f;
        r = run("save_game", {{"slot", "dug"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["tiles"] == 1);
        scene.terrain.restore({1, 0}, dented1, 0);
        // Closed: the ground as authored, with its versions (nothing reads as edited).
        GIANT_CHECK(run("close_game").ok);
        GIANT_CHECK(*scene.terrain.find({0, 0}) == authored && scene.terrain.version({0, 0}) == v0);
        GIANT_CHECK(physics.game_ground().tiles() == 0 && run("keep_damage").ok);
        // A tile not in the scene yet waits for it (a first save has tile 0's dents only now).
        r = run("load_game", {{"slot", "dug"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["tiles"] == 1);
        GIANT_CHECK(run("close_game").ok);
        std::optional<world::Terrain::Heights> gone = scene.terrain.remove({0, 0});
        r = run("load_game", {{"slot", "dug"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["ground_waiting"] == 1 && r.value["game_tiles"] == 0);
        GIANT_CHECK(!run("keep_damage").ok);
        scene.terrain.set({0, 0}, std::move(*gone));
        frame();
        GIANT_CHECK(saves.ground_waiting() == 0 && physics.game_ground().tiles() == 1);
        GIANT_CHECK(*scene.terrain.find({0, 0}) == dented0);
        GIANT_CHECK(run("close_game").ok);
        GIANT_CHECK(*scene.terrain.find({0, 0}) == authored);
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

GIANT_TEST("game saves: the bodies simulating moved are put back where they were; autosave while playing") {
    const std::filesystem::path folder = fresh_folder();
    {
        PhysicsState physics;
        PlayState play{physics};
        WorldStreaming streaming;
        GameSaves saves;
        EditorState state;
        state.physics = &physics;
        state.play = &play;
        state.world = &streaming;
        state.saves = &saves;
        ai::CommandRegistry commands;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_save_commands(commands, state);
        const auto run = [&](const char* name, const Json& args = Json::object()) {
            return commands.execute(name, args, {});
        };
        world::Scene& scene = state.scene;
        world::Entity floor;
        floor.shape = world::Shape::plane;
        floor.transform.position = {32.0, 0.0, 32.0};
        floor.transform.scale = {60.0f, 1.0f, 60.0f};
        scene.add(floor);
        world::Entity crate;
        crate.shape = world::Shape::cube;
        crate.body = world::BodyKind::dynamic;
        crate.transform.position = {30.0, 3.0, 30.0};
        const world::EntityId id = scene.add(crate);
        GIANT_CHECK(streaming.create(folder, scene, state.scene_generation, {}).error.empty());
        const auto frame = [&](double seconds = 1.0 / 60.0) {
            physics.update(scene, scene.take_changes(), 1.0 / 60.0);
            saves.update(state, seconds);
        };
        frame();
        // Simulating (not playing): the crate falls and is pushed aside.
        GIANT_CHECK(run("simulate", {{"on", true}}).ok);
        GIANT_CHECK(run("step_physics", {{"seconds", 2}}).ok);
        const world::Transform* pose = &physics.bodies().poses().at(id);
        GIANT_CHECK(std::fabs(pose->position.y - 0.5) < 0.05);
        const Quat turned = normalize(Quat{0.0f, 0.38268343f, 0.0f, 0.92387953f});
        GIANT_CHECK(physics.place_body(id, {40.0, 0.5, 25.0}, turned));
        const DVec3 saved_at{40.0, 0.5, 25.0};
        ai::CommandResult r = run("save_game", {{"slot", "crate"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["bodies"] == 1);
        // Stopped: the crate back where the scene has it.
        GIANT_CHECK(run("simulate", {{"on", false}}).ok);
        frame();
        GIANT_CHECK(physics.bodies().poses().empty());
        // Loaded: simulating again, the crate where it was saved (a share a frame, once its body is made).
        r = run("load_game", {{"slot", "crate"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["bodies"] == 1 && physics.simulating());
        for (int i = 0; i < 4 && saves.bodies_waiting() > 0; ++i) frame();
        GIANT_CHECK(saves.bodies_waiting() == 0 && saves.status().bodies_applied == 1);
        GIANT_CHECK(physics.bodies().poses().count(id) == 1);
        const DVec3 now = physics.bodies().poses().at(id).position;
        GIANT_CHECK(std::fabs(now.x - saved_at.x) < 0.01 && std::fabs(now.z - saved_at.z) < 0.01);
        // Moved in the world since: not put back (not this save's).
        GIANT_CHECK(run("simulate", {{"on", false}}).ok);
        GIANT_CHECK(run("set_entity", {{"id", id}, {"position", {20.0, 3.0, 20.0}}}).ok);
        frame();
        r = run("load_game", {{"slot", "crate"}, {"wait", true}});
        GIANT_CHECK(r.ok && saves.bodies_waiting() == 0 && saves.status().bodies_skipped == 1);
        GIANT_CHECK(physics.bodies().poses().count(id) == 0);
        // Deleted from the world since: it waits (it may yet stream in) and holds nothing else back.
        GIANT_CHECK(run("delete_entities", {{"ids", {id}}}).ok);
        frame();
        r = run("load_game", {{"slot", "crate"}, {"wait", true}});
        GIANT_CHECK(r.ok && saves.bodies_waiting() == 1 && physics.simulating());
        // Autosave: while playing, every autosave_seconds of play, its own base (a user's save stays of changes).
        r = run("save_game", {{"slot", "mine"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["base_slot"] == "mine");
        GIANT_CHECK(run("set_autosave", {{"seconds", 10}}).ok && !run("set_autosave", {{"seconds", -1}}).ok);
        state.camera.position = {32.0, 5.0, 32.0};
        GIANT_CHECK(run("play", {{"on", true}}).ok);
        for (int i = 0; i < 30 && play.waiting_for_ground(); ++i) frame();
        frame(5.0);
        GIANT_CHECK(saves.status().autosaves == 0 && !saves.busy()); // 5 s of play
        frame(6.0);
        saves.wait();
        frame();
        GIANT_CHECK(saves.status().autosaves == 1 && saves.status().autosave_error.empty());
        GIANT_CHECK(saves.status().bodies == 1 && saves.bodies_waiting() == 1); // (the waiting body kept in it)
        GIANT_CHECK(std::filesystem::exists(folder / "saves" / "autosave" / "save.json"));
        frame(1.0);
        GIANT_CHECK(saves.status().autosaves == 1); // not again until 10 s more
        r = run("save_game", {{"slot", "mine"}, {"wait", true}});
        GIANT_CHECK(r.ok && r.value["saves"] == 3 && r.value["base_slot"] == "mine");
        GIANT_CHECK(run("set_autosave", {{"seconds", 0}}).ok);
        frame(100.0);
        GIANT_CHECK(saves.status().autosaves == 1 && !saves.busy());
        // A bad bodies section is refused whole.
        GIANT_CHECK(run("play", {{"on", false}}).ok);
        const std::filesystem::path sections = folder / "saves" / "mine" / "sections";
        for (const auto& e : std::filesystem::directory_iterator(sections)) {
            if (e.path().filename().string().rfind("bodies.", 0) != 0) continue;
            std::ofstream(e.path(), std::ios::binary) << "{\"simulating\": true, \"poses\": [{\"entity\": 1, "
                                                          "\"position\": [0, 0, 0], \"rotation\": [0, 0, 0, 2], "
                                                          "\"of\": {\"position\": [0, 0, 0], \"rotation\": [0, 0, 0, 1]}}]}";
        }
        r = run("load_game", {{"slot", "mine"}, {"wait", true}});
        GIANT_CHECK(!r.ok);
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

GIANT_TEST("game saves: the player is put back as it was: on a wall, in the air, and where stopping returns to") {
    const std::filesystem::path folder = fresh_folder();
    {
        PhysicsState physics;
        PlayState play{physics};
        WorldStreaming streaming;
        GameSaves saves;
        EditorState state;
        state.physics = &physics;
        state.play = &play;
        state.world = &streaming;
        state.saves = &saves;
        ai::CommandRegistry commands;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_save_commands(commands, state);
        const auto run = [&](const char* name, const Json& args = Json::object()) {
            return commands.execute(name, args, {});
        };
        world::Scene& scene = state.scene;
        world::Entity floor;
        floor.shape = world::Shape::plane;
        floor.transform.position = {32.0, 0.0, 32.0};
        floor.transform.scale = {60.0f, 1.0f, 60.0f};
        scene.add(floor);
        world::Entity wall; // a 6 m block, its -x face at x = 38
        wall.shape = world::Shape::cube;
        wall.transform.position = {40.0, 3.0, 32.0};
        wall.transform.scale = {4.0f, 6.0f, 8.0f};
        scene.add(wall);
        GIANT_CHECK(streaming.create(folder, scene, state.scene_generation, {}).error.empty());
        const auto frame = [&] {
            play.before_step(scene, state.camera, state.scene_generation, 1.0 / 60.0);
            physics.update(scene, scene.take_changes(), 1.0 / 60.0);
            play.after_step(state.camera, 1.0 / 60.0);
            saves.update(state, 1.0 / 60.0);
        };
        frame();
        state.camera.position = {20.0, 5.0, 32.0};
        const EditorCamera before = state.camera; // where stopping goes back to
        GIANT_CHECK(run("play", {{"on", true}}).ok);
        for (int i = 0; i < 30 && play.waiting_for_ground(); ++i) frame();
        GIANT_CHECK(!play.waiting_for_ground());
        // On the wall, 2 m up.
        sim::PhysicsWorld& w = physics.world();
        const auto character = [&] { return *play.player(); };
        ai::CommandResult r = run("step_player", {{"seconds", 0.2}});
        GIANT_CHECK(r.ok);
        // (The player's handle is PlayState's own: put it there by its character.)
        const auto put = [&](const DVec3& at) {
            for (sim::CharacterId c = 1; c < 16; ++c) {
                if (!w.contains_character(c)) continue;
                w.set_character_position(c, at);
                return c;
            }
            return sim::k_no_character;
        };
        const sim::CharacterId me = put({37.65, 2.0, 32.0});
        GIANT_CHECK(me != sim::k_no_character && w.take_hold(me, {-1.0f, 0.0f, 0.0f}));
        GIANT_CHECK(character().climbing);
        r = run("save_game", {{"slot", "wall"}, {"wait", true}});
        GIANT_CHECK(r.ok);
        GIANT_CHECK(run("play", {{"on", false}}).ok);
        GIANT_CHECK(!play.playing() && !physics.simulating());
        state.camera.position = {0.0, 50.0, 0.0};
        r = run("load_game", {{"slot", "wall"}, {"wait", true}});
        GIANT_CHECK(r.ok && play.playing());
        for (int i = 0; i < 30 && play.waiting_for_ground(); ++i) frame();
        sim::CharacterState s = character();
        GIANT_CHECK(s.climbing && std::fabs(s.position.x - 37.65) < 0.05 && std::fabs(s.position.y - 2.0) < 0.1);
        // Stopping goes back where the saved game's play began, simulating as it was then.
        GIANT_CHECK(run("play", {{"on", false}}).ok);
        GIANT_CHECK(state.camera.position == before.position && !physics.simulating());
        // In the air, falling: it goes on falling from there.
        GIANT_CHECK(run("play", {{"on", true}}).ok);
        for (int i = 0; i < 30 && play.waiting_for_ground(); ++i) frame();
        const sim::CharacterId me2 = put({25.0, 10.0, 32.0});
        w.set_character_velocity(me2, {2.0f, -6.0f, 0.0f});
        r = run("save_game", {{"slot", "air"}, {"wait", true}});
        GIANT_CHECK(r.ok);
        GIANT_CHECK(run("play", {{"on", false}}).ok);
        r = run("load_game", {{"slot", "air"}, {"wait", true}});
        GIANT_CHECK(r.ok && play.playing());
        for (int i = 0; i < 30 && play.waiting_for_ground(); ++i) frame();
        s = character();
        GIANT_CHECK(!s.on_ground && std::fabs(s.position.y - 10.0) < 0.3 && s.velocity.y < -5.0f && s.velocity.x > 1.5f);
        // A bad player section is refused whole.
        GIANT_CHECK(run("play", {{"on", false}}).ok);
        for (const auto& e : std::filesystem::directory_iterator(folder / "saves" / "air" / "sections")) {
            if (e.path().filename().string().rfind("player.", 0) == 0)
                std::ofstream(e.path(), std::ios::binary) << "{\"feet\": [0, 0, 0]}"; // (missing keys: refused)
        }
        GIANT_CHECK(!run("load_game", {{"slot", "air"}, {"wait", true}}).ok);
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}
