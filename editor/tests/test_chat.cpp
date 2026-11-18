// The chat with Claude, as far as it needs no .NET: the tools Claude works
// with (searching, describing and running the editor's commands) and the
// chat's own commands. The conversation is tested in C#
// (tests/managed/ChatTests) and end to end by tools/mcp_smoke.py.
#include "../src/chat.h"
#include "../src/clock_state.h"
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include <string>
#include <utility>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

// Stands in for the conversation: keeps what it was asked.
struct FakeLink final : ChatLink {
    std::vector<std::pair<std::string, Json>> asked;
    ai::CommandResult ask(std::string_view what, const Json& args) override {
        asked.emplace_back(std::string(what), args);
        if (what == "send" && args["text"] == "fail") {
            return ai::CommandResult::failure("no key");
        }
        return ai::CommandResult::success({{"heard", std::string(what)}});
    }
};

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    ClockState clock;
    EditorState state;
    FakeLink link;
    Fixture() {
        state.meshes = &meshes;
        state.clock = &clock;
        register_scene_commands(commands, state);
        register_clock_commands(commands, state);
        register_chat_commands(commands, state);
    }
    ChatToolReply tool(const char* name, const Json& input) { return run_chat_tool(commands, name, input); }
    Json value(const char* name, const Json& input) {
        const ChatToolReply reply = tool(name, input);
        GIANT_CHECK(reply.ok);
        return ai::parse_json(reply.text).value_or(Json::object());
    }
};

bool lists(const Json& found, const char* name) {
    for (const Json& c : found["commands"]) {
        if (c["name"] == name) {
            return true;
        }
    }
    return false;
}

} // namespace

GIANT_TEST("chat tools: every tool has a name, a description and an object schema") {
    const Json tools = chat_tools();
    GIANT_CHECK(tools.is_array() && tools.size() == 3);
    for (const Json& tool : tools) {
        GIANT_CHECK(ai::is_valid_command_name(tool["name"].get<std::string>()));
        GIANT_CHECK(tool["description"].get<std::string>().size() > 40);
        GIANT_CHECK(tool["input_schema"]["type"] == "object" && tool["input_schema"]["properties"].is_object());
        for (const Json& needed : tool["input_schema"]["required"]) {
            GIANT_CHECK(tool["input_schema"]["properties"].contains(needed.get<std::string>()));
        }
    }
}

GIANT_TEST("chat tools: find_commands searches names and descriptions, best first, never the chat itself") {
    Fixture f;
    Json found = f.value("find_commands", {{"words", "time of day"}});
    GIANT_CHECK(lists(found, "set_time") && lists(found, "get_time"));
    // A word in the name counts for more than one in a description.
    GIANT_CHECK(found["commands"][0]["name"] == "set_time" || found["commands"][0]["name"] == "get_time");
    GIANT_CHECK(found["of"] == f.commands.commands().size() && found["matched"].get<usize>() >= 2);
    for (const Json& c : found["commands"]) {
        GIANT_CHECK(c["about"].get<std::string>().size() <= 160 && !c["about"].get<std::string>().empty());
        if (c["name"] == "get_time") {
            GIANT_CHECK(c["changes_nothing"] == true);
        }
        if (c["name"] == "set_time") {
            GIANT_CHECK(c["changes_nothing"] == false);
        }
    }
    // Capitals, punctuation and a plural's "s" do not matter.
    found = f.value("find_commands", {{"words", "Entities, please!"}});
    GIANT_CHECK(found["matched"].get<usize>() > 0);
    bool entity = false;
    for (const Json& c : found["commands"]) {
        entity = entity || c["name"].get<std::string>().find("entit") != std::string::npos;
    }
    GIANT_CHECK(entity);
    // The most asked for, and no more than the most there is.
    found = f.value("find_commands", {{"words", "the a scene entity"}, {"limit", 3}});
    GIANT_CHECK(found["commands"].size() == 3 && found["matched"].get<usize>() > 3);
    found = f.value("find_commands", {{"words", "the a scene entity"}, {"limit", 100000}});
    GIANT_CHECK(found["commands"].size() <= k_chat_most_found);
    // Nothing fits: an empty list, not a failure (Claude tries other words).
    found = f.value("find_commands", {{"words", "zzzqqq"}});
    GIANT_CHECK(found["commands"].empty() && found["matched"] == 0);
    // The chat's own commands are not Claude's to find.
    found = f.value("find_commands", {{"words", "chat claude"}, {"limit", 50}});
    GIANT_CHECK(!lists(found, "chat_send") && !lists(found, "get_chat") && !lists(found, "stop_chat") && !lists(found, "clear_chat"));
    GIANT_CHECK(!f.tool("find_commands", Json::object()).ok);
    GIANT_CHECK(!f.tool("find_commands", {{"words", " ,. "}}).ok);
    GIANT_CHECK(!f.tool("find_commands", {{"words", 3}}).ok);
}

GIANT_TEST("chat tools: describe_command gives the description and the arguments' schema") {
    Fixture f;
    const Json described = f.value("describe_command", {{"name", "set_time"}});
    GIANT_CHECK(described["name"] == "set_time" && described["changes_nothing"] == false);
    GIANT_CHECK(described["description"] == f.commands.find("set_time")->description);
    GIANT_CHECK(described["arguments"]["properties"].contains("hours"));
    ChatToolReply reply = f.tool("describe_command", {{"name", "no_such_command"}});
    GIANT_CHECK(!reply.ok && reply.text.find("find_commands") != std::string::npos);
    GIANT_CHECK(!f.tool("describe_command", {{"name", "chat_send"}}).ok);
    GIANT_CHECK(!f.tool("describe_command", Json::object()).ok);
}

GIANT_TEST("chat tools: run_command runs as the person would: checked, answered, on the undo history") {
    Fixture f;
    Json ran = f.value("run_command", {{"name", "set_time"}, {"args", {{"on", true}, {"hours", 6.5}}}});
    GIANT_CHECK(f.state.scene.clock.on && f.state.scene.clock.hours == 6.5);
    GIANT_CHECK(f.commands.can_undo());
    // No arguments: left out, or null.
    ran = f.value("run_command", {{"name", "get_time"}});
    GIANT_CHECK(ran["clock"]["hours"] == 6.5);
    ran = f.value("run_command", {{"name", "get_time"}, {"args", nullptr}});
    GIANT_CHECK(ran["clock"]["on"] == true);
    // Claude can undo its own work.
    GIANT_CHECK(f.tool("run_command", {{"name", "undo"}}).ok);
    GIANT_CHECK(!f.state.scene.clock.on);
    // A refused command's error goes back, and nothing is changed.
    ChatToolReply reply = f.tool("run_command", {{"name", "set_time"}, {"args", {{"hours", 25}}}});
    GIANT_CHECK(!reply.ok && reply.text.find("hours") != std::string::npos);
    GIANT_CHECK(!f.state.scene.clock.on);
    reply = f.tool("run_command", {{"name", "no_such_command"}});
    GIANT_CHECK(!reply.ok && reply.text.find("no_such_command") != std::string::npos);
    GIANT_CHECK(!f.tool("run_command", {{"name", "set_time"}, {"args", "on"}}).ok);
    GIANT_CHECK(!f.tool("run_command", {{"args", Json::object()}}).ok);
    // Not Claude's to run: the chat itself (it would talk to itself).
    f.state.chat = &f.link;
    for (const char* name : {"chat_send", "get_chat", "stop_chat", "clear_chat"}) {
        GIANT_CHECK(!f.tool("run_command", {{"name", name}, {"args", {{"text", "hi"}}}}).ok);
    }
    GIANT_CHECK(f.link.asked.empty());
    // An unknown tool, or an input that is not an object.
    GIANT_CHECK(!f.tool("delete_everything", Json::object()).ok);
    GIANT_CHECK(!f.tool("run_command", Json::array()).ok);
}

GIANT_TEST("chat tools: closing the editor is not Claude's to do") {
    ai::CommandRegistry commands;
    bool quit = false;
    GIANT_CHECK(commands.add({"quit_editor", "Closes the editor.", Json{{"type", "object"}}, false,
                              [&quit](const Json&, ai::CommandContext&) {
                                  quit = true;
                                  return ai::CommandResult::success();
                              }}));
    const ChatToolReply reply = run_chat_tool(commands, "run_command", {{"name", "quit_editor"}});
    GIANT_CHECK(!reply.ok && !quit && reply.text.find("person") != std::string::npos);
    GIANT_CHECK(!run_chat_tool(commands, "describe_command", {{"name", "quit_editor"}}).ok);
}

GIANT_TEST("chat commands: each asks the conversation; without one they say so") {
    Fixture f;
    ai::CommandResult r = f.commands.execute("chat_send", {{"text", "hi"}});
    GIANT_CHECK(!r.ok && r.error.find("not available") != std::string::npos);
    GIANT_CHECK(!f.commands.execute("get_chat").ok);
    f.state.chat = &f.link;
    r = f.commands.execute("chat_send", {{"text", "plant a tree"}});
    GIANT_CHECK(r.ok && r.value["heard"] == "send");
    GIANT_CHECK(f.commands.execute("get_chat", {{"after", 3}, {"limit", 10}}).ok);
    GIANT_CHECK(f.commands.execute("stop_chat").ok);
    GIANT_CHECK(f.commands.execute("clear_chat").ok);
    GIANT_CHECK(f.link.asked.size() == 4);
    GIANT_CHECK(f.link.asked[0].first == "send" && f.link.asked[0].second["text"] == "plant a tree");
    GIANT_CHECK(f.link.asked[1].first == "read" && f.link.asked[1].second["after"] == 3 && f.link.asked[1].second["limit"] == 10);
    GIANT_CHECK(f.link.asked[2].first == "stop" && f.link.asked[3].first == "clear");
    // The conversation's refusal is the command's.
    r = f.commands.execute("chat_send", {{"text", "fail"}});
    GIANT_CHECK(!r.ok && r.error == "no key");
    // Checked before they are asked.
    GIANT_CHECK(!f.commands.execute("chat_send", {{"text", ""}}).ok);
    GIANT_CHECK(!f.commands.execute("chat_send").ok);
    GIANT_CHECK(!f.commands.execute("get_chat", {{"limit", 0}}).ok);
    GIANT_CHECK(f.link.asked.size() == 5);
    // The chat is not part of the scene: nothing of it is on the undo history.
    GIANT_CHECK(!f.commands.can_undo());
    GIANT_CHECK(f.commands.find("get_chat")->read_only && !f.commands.find("chat_send")->read_only);
}
