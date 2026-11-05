// The scene commands, run the way MCP and scripts run them.
#include "../src/editor_commands.h"

#include "giant_test.h"

#include <filesystem>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {
struct Fixture {
    ai::CommandRegistry commands;
    EditorState state;
    Fixture() { register_scene_commands(commands, state); }
    ai::CommandResult run(const char* name, const Json& args = Json::object(), bool record = true) {
        ai::ExecuteOptions options;
        options.record_history = record;
        return commands.execute(name, args, options);
    }
};
std::string temp_scene(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
} // namespace

GIANT_TEST("editor: undo skips changes made to a scene a script has since replaced") {
    Fixture f;
    const auto created = f.run("create_entity", {{"name", "a"}});
    GIANT_CHECK(created.ok);
    const u64 id = created.value["id"].get<u64>();
    const std::string path = temp_scene("giant_test_generation.scene.json");
    GIANT_CHECK(f.run("save_scene", {{"path", path}}).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"name", "b"}}).ok);

    // A script loads the saved file (same id, outside the history) and edits it.
    GIANT_CHECK(f.run("load_scene", {{"path", path}}, false).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"name", "c"}}, false).ok);

    // The recorded steps belong to the old scene: they must not touch this one.
    GIANT_CHECK(f.commands.undo()); // set_entity "b"
    GIANT_CHECK(f.state.scene.find(id) != nullptr && f.state.scene.find(id)->name == "c");
    GIANT_CHECK(f.commands.undo()); // save_scene records nothing; this is create_entity
    GIANT_CHECK(f.state.scene.find(id) != nullptr);
    std::filesystem::remove(path);
}

GIANT_TEST("editor: undo and redo across a recorded scene swap still apply") {
    Fixture f;
    const u64 id = f.run("create_entity", {{"name", "a"}}).value["id"].get<u64>();
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"name", "b"}}).ok);
    GIANT_CHECK(f.run("new_scene").ok);
    GIANT_CHECK(f.state.scene.entities().empty());
    GIANT_CHECK(f.commands.undo()); // new_scene: the old scene and its generation return
    GIANT_CHECK(f.commands.undo()); // set_entity
    GIANT_CHECK(f.state.scene.find(id)->name == "a");
    GIANT_CHECK(f.commands.redo());
    GIANT_CHECK(f.state.scene.find(id)->name == "b");
    GIANT_CHECK(f.commands.redo());
    GIANT_CHECK(f.state.scene.entities().empty());
    GIANT_CHECK(f.commands.undo());
    GIANT_CHECK(f.state.scene.find(id)->name == "b");
}

GIANT_TEST("editor: creating entities fails cleanly when every id is used") {
    Fixture f;
    f.state.scene.reserve_ids_below(world::k_max_entity_id); // one id left
    const auto batch = f.run("create_entities", {{"entities", Json::array({Json::object(), Json::object()})}});
    GIANT_CHECK(!batch.ok);
    GIANT_CHECK(batch.error.find("every entity id has been used") != std::string::npos);
    GIANT_CHECK(f.state.scene.entities().empty()); // all or nothing
    GIANT_CHECK(!f.commands.can_undo());

    // Ids are never reused, so the failed batch spent the last one.
    GIANT_CHECK(!f.run("create_entity").ok);
}

GIANT_TEST("editor: the last entity id can be used") {
    Fixture f;
    f.state.scene.reserve_ids_below(world::k_max_entity_id);
    const auto last = f.run("create_entity");
    GIANT_CHECK(last.ok && last.value["id"].get<u64>() == world::k_max_entity_id);
    GIANT_CHECK(!f.run("create_entity").ok);
    GIANT_CHECK(f.state.scene.entities().size() == 1);
}
