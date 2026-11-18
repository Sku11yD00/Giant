// The chat with Claude, as far as it needs no .NET: the tools Claude works
// with (searching, describing and running the editor's commands) and the
// chat's own commands. The conversation is tested in C#
// (tests/managed/ChatTests) and end to end by tools/chat_smoke.py.
#include "../src/chat.h"
#include "../src/clock_state.h"
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include <algorithm>
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
    GIANT_CHECK(found["of"] == f.commands.commands().size() - 4 && found["matched"].get<usize>() >= 2); // (not the chat's four)
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
    const usize matched = found["matched"].get<usize>();
    GIANT_CHECK(f.value("find_commands", {{"words", "scene entity"}})["matched"] == matched); // ("the" and "a" find nothing)
    found = f.value("find_commands", {{"words", "the a scene entity"}, {"limit", 100000}});
    GIANT_CHECK(found["commands"].size() == std::min(matched, k_chat_most_found));
    GIANT_CHECK(f.value("find_commands", {{"words", "scene entity"}, {"limit", 2.0}})["commands"].size() == 2);
    GIANT_CHECK(f.value("find_commands", {{"words", "scene entity"}, {"limit", -5}})["commands"].size() == 1);
    GIANT_CHECK(f.value("find_commands", {{"words", "scene entity"}, {"limit", 18446744073709551615ull}})["commands"].size() ==
                std::min(matched, k_chat_most_found));
    // A word is found where a word starts with it, not inside one: "tity" is in no command, "ent" in many.
    GIANT_CHECK(f.value("find_commands", {{"words", "tity"}})["matched"] == 0);
    GIANT_CHECK(f.value("find_commands", {{"words", "ent"}})["matched"].get<usize>() > 0);
    // Nothing but small words: refused, with why.
    GIANT_CHECK(!f.tool("find_commands", {{"words", "the of a"}}).ok);
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

namespace {

// A conversation of numbered lines, read as get_chat reads the real one.
struct LinesLink final : ChatLink {
    u64 oldest = 1;
    u64 latest = 0;
    bool started = true;
    int reads = 0;
    int fail_read = 0;   // the read (counted from 1) that fails; 0: none
    bool mistyped = false; // every field of the reply of another type than it should be
    bool stuck = false;    // the reply's `after` does not move on (a chat gone wrong)
    ai::CommandResult ask(std::string_view what, const Json& args) override {
        if (what != "read") {
            return ai::CommandResult::success();
        }
        ++reads;
        if (reads == fail_read) {
            return ai::CommandResult::failure("the chat failed");
        }
        if (mistyped) {
            return ai::CommandResult::success({{"entries", Json::array({{{"serial", "9"}, {"kind", 3}, {"text", nullptr}, {"tool", nullptr}, {"ok", 1}},
                                                                         "not a line", nullptr})},
                                               {"after", "x"}, {"latest", nullptr}, {"oldest", -5}, {"busy", "no"},
                                               {"started", 1},  {"key_found", "yes"}, {"model", nullptr},
                                               {"input_tokens", -1}, {"output_tokens", 2.5}});
        }
        const u64 after = args.value("after", u64{0});
        const u64 limit = args.value("limit", u64{64});
        Json entries = Json::array();
        u64 cursor = after;
        for (u64 serial = std::max(after + 1, oldest); serial <= latest && entries.size() < limit; ++serial) {
            Json e = {{"serial", serial}, {"kind", serial % 2 == 1 ? "you" : "result"}, {"text", "line " + std::to_string(serial)}};
            if (serial % 2 == 0) {
                e["tool"] = "run_command";
                e["ok"] = serial % 4 == 0;
            }
            entries.push_back(std::move(e));
            cursor = serial;
        }
        Json reply = {{"entries", std::move(entries)}, {"after", stuck ? after : std::min(cursor, latest)}, {"latest", latest},
                      {"oldest", oldest},              {"busy", false},                     {"started", started}};
        if (started) {
            reply.update({{"key_found", true}, {"model", "claude-opus-5-5"}, {"input_tokens", 120}, {"output_tokens", 30}});
        }
        return ai::CommandResult::success(std::move(reply));
    }
};

} // namespace

GIANT_TEST("chat view: reads the conversation only when it changed, and follows clears and dropped lines") {
    Fixture f;
    LinesLink link;
    f.state.chat = &link;
    ChatView view;
    // Before any message: read once, nothing there, nothing started.
    link.started = false;
    GIANT_CHECK(!view.update(f.commands, f.state) && link.reads == 1);
    GIANT_CHECK(view.lines().empty() && !view.started() && !view.key_found() && view.model().empty());
    GIANT_CHECK(!view.update(f.commands, f.state) && link.reads == 1); // nothing changed: not read again
    // Lines arrive.
    link.started = true;
    link.latest = 3;
    f.state.chat_stamp = 1;
    GIANT_CHECK(view.update(f.commands, f.state) && link.reads == 2);
    GIANT_CHECK(view.lines().size() == 3 && view.lines()[0].serial == 1 && view.lines()[0].kind == "you" && view.lines()[0].text == "line 1");
    GIANT_CHECK(view.lines()[1].kind == "result" && view.lines()[1].tool == "run_command" && !view.lines()[1].ok);
    GIANT_CHECK(view.started() && view.key_found() && view.model() == "claude-opus-5-5" && view.input_tokens() == 120 && view.output_tokens() == 30);
    for (int i = 0; i < 100; ++i) {
        GIANT_CHECK(!view.update(f.commands, f.state));
    }
    GIANT_CHECK(link.reads == 2);
    // Only what is new is read; a stamp that moved with nothing new adds nothing.
    link.latest = 5;
    f.state.chat_stamp = 2;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().size() == 5 && view.lines()[4].serial == 5 && view.lines()[3].ok);
    f.state.chat_stamp = 3;
    const int reads_then = link.reads;
    GIANT_CHECK(!view.update(f.commands, f.state) && view.lines().size() == 5 && link.reads == reads_then + 1);
    // Many at once: read in pages, all in one go while they are few pages.
    link.latest = 5 + 600;
    f.state.chat_stamp = 4;
    const int before = link.reads;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().size() == 605 && link.reads == before + 3);
    for (usize i = 0; i < view.lines().size(); ++i) {
        GIANT_CHECK(view.lines()[i].serial == i + 1); // none skipped, none twice
    }
    // Cleared: the conversation keeps only a new line; the old ones go from the view too.
    link.oldest = link.latest = 606;
    f.state.chat_stamp = 5;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().size() == 1 && view.lines()[0].serial == 606);
    // More than a few pages at once: the rest at the next update; never more kept than the most.
    link.oldest = 606;
    link.latest = 606 + 3000;
    f.state.chat_stamp = 6;
    GIANT_CHECK(view.update(f.commands, f.state));
    GIANT_CHECK(view.lines().size() == ChatView::k_most_lines && view.lines().back().serial == 606 + 8 * 256); // (eight pages, the newest kept)
    GIANT_CHECK(view.update(f.commands, f.state)); // (the stamp did not move: it reads on by itself)
    GIANT_CHECK(view.lines().size() == ChatView::k_most_lines && view.lines().back().serial == 3606);
    GIANT_CHECK(view.lines().front().serial == 3606 - ChatView::k_most_lines + 1);
    GIANT_CHECK(!view.update(f.commands, f.state));
    // The conversation dropped its oldest lines: so does the view.
    link.oldest = 3000;
    f.state.chat_stamp = 7;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().front().serial == 3000 && view.lines().back().serial == 3606);
    // A read that fails part way through many pages: what was read is kept, and the rest is read at the next update.
    const usize kept = view.lines().size();
    link.latest = 3606 + 600;
    link.fail_read = link.reads + 2;
    f.state.chat_stamp = 8;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().back().serial == 3606 + 256);
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().back().serial == 3606 + 600);
    GIANT_CHECK(view.lines().size() == std::min(kept + 600, ChatView::k_most_lines));
    for (usize i = 1; i < view.lines().size(); ++i) {
        GIANT_CHECK(view.lines()[i].serial == view.lines()[i - 1].serial + 1);
    }
    // A reply with every field of the wrong type: read as nothing, without a throw.
    link.mistyped = true;
    f.state.chat_stamp = 9;
    const usize before_bad = view.lines().size();
    view.update(f.commands, f.state);
    GIANT_CHECK(view.lines().size() == before_bad + 1 && view.lines().back().serial == 0 && view.lines().back().kind.empty() &&
                view.lines().back().text.empty() && view.lines().back().ok);
    GIANT_CHECK(!view.started() && !view.key_found() && view.model().empty() && view.input_tokens() == 0 && view.output_tokens() == 0);
    link.mistyped = false;
    // No chat at all (a tool without one): nothing new, nothing lost, and no failure; and it reads again when there is one.
    f.state.chat = nullptr;
    f.state.chat_stamp = 10;
    GIANT_CHECK(!view.update(f.commands, f.state) && view.lines().size() == before_bad + 1);
    f.state.chat = &link;
    link.latest += 1;
    f.state.chat_stamp = 11;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().back().serial == link.latest && view.started());
}

GIANT_TEST("chat view: a full page is followed by one more read, and a chat that does not move on is not read in a loop") {
    Fixture f;
    LinesLink link;
    f.state.chat = &link;
    ChatView view;
    // Exactly a page: the second read finds nothing more.
    link.latest = 256;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().size() == 256 && link.reads == 2);
    GIANT_CHECK(!view.update(f.commands, f.state) && link.reads == 2);
    // A full page whose reply does not say where it got to: one read, and no more until something changes.
    link.latest = 256 + 600;
    link.stuck = true;
    f.state.chat_stamp = 1;
    GIANT_CHECK(view.update(f.commands, f.state) && link.reads == 3);
    GIANT_CHECK(!view.update(f.commands, f.state) && link.reads == 3);
}

GIANT_TEST("chat view: an unstarted chat's answer (the bridge's own) reads as empty, then the started one's lines") {
    Fixture f;
    struct Unstarted final : ChatLink {
        bool started = false;
        ai::CommandResult ask(std::string_view, const Json&) override {
            if (!started) { // (as ChatBridge::ask answers before .NET runs)
                return ai::CommandResult::success({{"entries", Json::array()}, {"after", 0}, {"latest", 0}, {"oldest", 1},
                                                   {"busy", false},           {"started", false}});
            }
            return ai::CommandResult::success({{"entries", Json::array({{{"serial", 1}, {"kind", "you"}, {"text", "hi"}}})},
                                               {"after", 1}, {"latest", 1}, {"oldest", 1}, {"busy", true}, {"started", true},
                                               {"key_found", true}, {"model", "m"}, {"input_tokens", 0}, {"output_tokens", 0}});
        }
    } link;
    f.state.chat = &link;
    ChatView view;
    GIANT_CHECK(!view.update(f.commands, f.state) && view.lines().empty() && !view.started());
    link.started = true;
    f.state.chat_stamp = 1;
    GIANT_CHECK(view.update(f.commands, f.state) && view.lines().size() == 1 && view.lines()[0].serial == 1 && view.lines()[0].ok);
    GIANT_CHECK(view.started() && view.key_found());
}

GIANT_TEST("chat stamp: moves when the conversation may have changed, never for a read") {
    ChatStamp stamp;
    GIANT_CHECK(stamp.value() == 0);
    stamp.asked("read");
    stamp.updated(0);
    GIANT_CHECK(stamp.value() == 0); // (or the panel, which reads when it moves, would read every frame)
    stamp.asked("send");
    GIANT_CHECK(stamp.value() == 1);
    stamp.updated(3); // a line, and Claude working
    GIANT_CHECK(stamp.value() == 2);
    for (int i = 0; i < 100; ++i) {
        stamp.updated(3);
        stamp.asked("read");
    }
    GIANT_CHECK(stamp.value() == 2);
    stamp.updated(2); // Claude done, no new line
    GIANT_CHECK(stamp.value() == 3);
    stamp.asked("stop");
    stamp.asked("clear");
    GIANT_CHECK(stamp.value() == 5);
}
