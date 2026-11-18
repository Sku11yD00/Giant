// The chat's commands (chat_send, get_chat, stop_chat, clear_chat) and the
// tools Claude works with (chat.h).
#include "chat.h"
#include "editor_commands.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "chat command could not be registered");
}

// What Claude may not run: the chat itself (it would be talking to itself),
// and closing the editor under the person.
constexpr std::array<std::string_view, 5> k_not_for_claude = {"chat_send", "get_chat", "stop_chat", "clear_chat",
                                                              "quit_editor"};

bool not_for_claude(std::string_view name) {
    return std::find(k_not_for_claude.begin(), k_not_for_claude.end(), name) != k_not_for_claude.end();
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// The words of a search: letters and digits, lower case; a plural's "s" dropped
// ("trees" finds "tree").
std::vector<std::string> words_of(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    const auto end_word = [&] {
        if (word.size() > 3 && word.back() == 's') {
            word.pop_back();
        }
        if (!word.empty()) {
            words.push_back(word);
        }
        word.clear();
    };
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            end_word();
        }
    }
    end_word();
    return words;
}

// A description's start, for a list: its first sentence, or 160 bytes.
std::string start_of(const std::string& description) {
    const usize stop = description.find(". ");
    usize length = std::min<usize>(stop == std::string::npos ? description.size() : stop + 1, 160);
    // (Not through the middle of a UTF-8 character.)
    while (length > 0 && length < description.size() && (static_cast<unsigned char>(description[length]) & 0xC0) == 0x80) {
        --length;
    }
    return description.substr(0, length);
}

ChatToolReply find_commands(const ai::CommandRegistry& registry, const Json& input) {
    if (!input.contains("words") || !input["words"].is_string()) {
        return {false, "find_commands needs `words`: what to look for"};
    }
    const std::vector<std::string> words = words_of(input["words"].get<std::string>());
    if (words.empty()) {
        return {false, "`words` holds nothing to look for"};
    }
    usize limit = 20;
    if (input.contains("limit") && input["limit"].is_number_integer()) {
        limit = static_cast<usize>(std::clamp<long long>(input["limit"].get<long long>(), 1, static_cast<long long>(k_chat_most_found)));
    }
    // A word in a command's name counts for more than one in its description.
    struct Found {
        int score = 0;
        usize index = 0;
    };
    std::vector<Found> found;
    const auto& commands = registry.commands();
    for (usize i = 0; i < commands.size(); ++i) {
        if (not_for_claude(commands[i].name)) {
            continue;
        }
        const std::string name = lower(commands[i].name);
        const std::string description = lower(commands[i].description);
        int score = 0;
        for (const std::string& word : words) {
            if (name.find(word) != std::string::npos) {
                score += 4;
            } else if (description.find(word) != std::string::npos) {
                score += 1;
            }
        }
        if (score > 0) {
            found.push_back({score, i});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.score > b.score; });
    Json list = Json::array();
    for (usize i = 0; i < found.size() && i < limit; ++i) {
        const ai::CommandDesc& c = commands[found[i].index];
        list.push_back({{"name", c.name}, {"about", start_of(c.description)}, {"changes_nothing", c.read_only}});
    }
    return {true, ai::to_json_text(Json{{"commands", std::move(list)}, {"matched", found.size()}, {"of", commands.size()}})};
}

ChatToolReply describe_command(const ai::CommandRegistry& registry, const Json& input) {
    if (!input.contains("name") || !input["name"].is_string()) {
        return {false, "describe_command needs `name`"};
    }
    const std::string name = input["name"].get<std::string>();
    const ai::CommandDesc* command = not_for_claude(name) ? nullptr : registry.find(name);
    if (command == nullptr) {
        return {false, "there is no command named \"" + name + "\" (find_commands searches them)"};
    }
    return {true, ai::to_json_text(Json{{"name", command->name},
                                        {"description", command->description},
                                        {"changes_nothing", command->read_only},
                                        {"arguments", command->input_schema}})};
}

ChatToolReply run_command(ai::CommandRegistry& registry, const Json& input) {
    if (!input.contains("name") || !input["name"].is_string()) {
        return {false, "run_command needs `name`"};
    }
    const std::string name = input["name"].get<std::string>();
    if (name == "quit_editor") {
        return {false, "closing the editor is the person's to do"};
    }
    if (not_for_claude(name) || registry.find(name) == nullptr) {
        return {false, "there is no command named \"" + name + "\" (find_commands searches them)"};
    }
    Json args = Json::object();
    if (input.contains("args") && !input["args"].is_null()) {
        if (!input["args"].is_object()) {
            return {false, "`args` must be an object: the command's arguments by name"};
        }
        args = input["args"];
    }
    // As the person would run it: checked against its schema, and on the undo history.
    const CommandResult result = registry.execute(name, args);
    if (!result.ok) {
        return {false, result.error};
    }
    Json value = result.value;
    if (!result.images.empty() && value.is_object()) {
        value["note_to_assistant"] = "The command returned a picture, which the chat cannot pass on yet.";
    }
    return {true, ai::to_json_text(value)};
}

} // namespace

Json chat_tools() {
    return Json::array(
        {{{"name", "find_commands"},
          {"description",
           "Searches the editor's commands by words and lists the best matches with what each is for. Call it first "
           "whenever you are not sure which command does something, and before saying something cannot be done. "
           "Search by what the thing is called in a game editor (\"tree\", \"weather rain\", \"save\", \"person "
           "routine\"); try other words when nothing fits."},
          {"input_schema",
           {{"type", "object"},
            {"properties",
             {{"words", {{"type", "string"}, {"description", "What to look for: one or a few words."}}},
              {"limit", {{"type", "integer"}, {"description", "The most commands to list (default 20, at most 50)."}}}}},
            {"required", Json::array({"words"})}}}},
         {{"name", "describe_command"},
          {"description",
           "A command's full description and the JSON Schema of its arguments. Call it before the first time you run "
           "a command in a conversation, so the arguments are right."},
          {"input_schema",
           {{"type", "object"},
            {"properties", {{"name", {{"type", "string"}, {"description", "The command's name, as find_commands gave it."}}}}},
            {"required", Json::array({"name"})}}}},
         {{"name", "run_command"},
          {"description",
           "Runs one of the editor's commands in the person's open editor and returns its result as JSON, or its "
           "error. Changes to the scene can be undone with the `undo` command; changes to files cannot."},
          {"input_schema",
           {{"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"description", "The command's name."}}},
              {"args", {{"type", "object"}, {"description", "The command's arguments by name, as describe_command's schema says. Leave out for none."}}}}},
            {"required", Json::array({"name"})}}}}});
}

ChatToolReply run_chat_tool(ai::CommandRegistry& registry, std::string_view name, const Json& input) {
    if (!input.is_object()) {
        return {false, "the tool's input must be an object"};
    }
    if (name == "find_commands") {
        return find_commands(registry, input);
    }
    if (name == "describe_command") {
        return describe_command(registry, input);
    }
    if (name == "run_command") {
        return run_command(registry, input);
    }
    return {false, "there is no tool named \"" + std::string(name) + "\""};
}

void register_chat_commands(ai::CommandRegistry& registry, EditorState& state) {
    const auto ask = [&state](std::string_view what, const Json& args) {
        if (state.chat == nullptr) {
            return CommandResult::failure("the chat is not available here");
        }
        return state.chat->ask(what, args);
    };

    add(registry,
        {"chat_send",
         "Sends a message to Claude in the editor's chat: the built-in assistant, which does what is asked with the "
         "editor's own commands (its work is on the undo history like the person's). Returns at once; the answer "
         "and what Claude does arrive over the next seconds: read them with get_chat (`busy` is true until it is "
         "done). One message at a time. Needs the person's Claude key: the ANTHROPIC_API_KEY environment variable, "
         "or the key file get_chat's error names; and the .NET runtime.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["text"],
             "properties": {"text": {"type": "string", "minLength": 1, "maxLength": 20000,
                                     "description": "What to say to Claude."}}})"),
         false, [ask](const Json& args, CommandContext&) { return ask("send", args); }});

    add(registry,
        {"get_chat",
         "Reads the chat with Claude: the lines after serial `after`, oldest first. A line's `kind` is \"you\" (the "
         "person), \"claude\" (its words), \"tool\" (a tool it called: `tool` names it, `text` is the arguments), "
         "\"result\" (what came back; `ok`), \"note\" or \"error\". Pass the reply's `after` as the next call's to "
         "read on. Also: `busy` (Claude is working), `key_found`, `model`, the tokens used so far, and `latest` and "
         "`oldest` (the newest line's serial and the oldest kept; 2,000 are).",
         Json::parse(R"({"type": "object", "additionalProperties": false,
             "properties": {"after": {"type": "integer", "minimum": 0, "maximum": 9007199254740991,
                                      "description": "Read the lines after this serial (default 0: from the oldest kept)."},
                            "limit": {"type": "integer", "minimum": 1, "maximum": 256,
                                      "description": "The most lines to return (default 64)."}}})"),
         true, [ask](const Json& args, CommandContext&) { return ask("read", args); }});

    add(registry,
        {"stop_chat",
         "Stops Claude's work on the last chat message: no further commands are run (one running now finishes). "
         "What it already did stays (undo takes it back).",
         Json{{"type", "object"}, {"additionalProperties", false}}, false,
         [ask](const Json& args, CommandContext&) { return ask("stop", args); }});

    add(registry,
        {"clear_chat",
         "Forgets the chat with Claude (stopping any work on it): the next message starts a new conversation. "
         "The scene is not changed.",
         Json{{"type", "object"}, {"additionalProperties", false}}, false,
         [ask](const Json& args, CommandContext&) { return ask("clear", args); }});
}

} // namespace giant::editor
