// The chat's commands (chat_send, get_chat, stop_chat, clear_chat) and the
// tools Claude works with (chat.h).
#include "chat.h"
#include "editor_commands.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
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

// Words that say nothing of what is looked for.
constexpr std::array<std::string_view, 14> k_small_words = {"the", "and", "for", "with", "from", "into", "that",
                                                            "this", "all", "any", "how", "can", "please", "command"};

// The words of a text: letters and digits, lower case; a plural's "s" dropped
// ("trees" finds "tree"). `searched`: without the words too short or too
// common to find anything by.
std::vector<std::string> words_of(std::string_view text, bool searched) {
    std::vector<std::string> words;
    std::string word;
    const auto end_word = [&] {
        if (word.size() > 3 && word.back() == 's') {
            word.pop_back();
        }
        const bool small = word.size() < 3 || std::find(k_small_words.begin(), k_small_words.end(), word) != k_small_words.end();
        if (!word.empty() && !(searched && small)) {
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

// The first `most` bytes of a text at most, not through the middle of a UTF-8 character.
std::string utf8_start(const std::string& text, usize most) {
    usize length = std::min(text.size(), most);
    while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return text.substr(0, length);
}

// A description's start, for a list: its first sentence, or 160 bytes.
std::string start_of(const std::string& description) {
    const usize stop = description.find(". ");
    return utf8_start(description, std::min<usize>(stop == std::string::npos ? description.size() : stop + 1, 160));
}

// Runs a command for Claude as the person would run it: checked against its schema, and on the undo history.
ChatToolReply execute_for_claude(ai::CommandRegistry& registry, const std::string& name, const Json& args) {
    CommandResult result = registry.execute(name, args);
    if (!result.ok) {
        return {false, result.error};
    }
    ChatToolReply reply;
    reply.ok = true;
    // The pictures go to Claude beside the result: those its service takes.
    usize left_out = 0;
    for (ai::CommandImage& image : result.images) {
        // (What it says it is, by the file's first bytes: a picture the service cannot read fails the whole request.)
        const bool kind = (image.mime_type == "image/png" && image.data.starts_with("\x89PNG\r\n\x1a\n")) ||
                          (image.mime_type == "image/jpeg" && image.data.starts_with("\xFF\xD8\xFF"));
        if (kind && image.data.size() <= k_chat_most_image_bytes && reply.images.size() < k_chat_most_images) {
            reply.images.push_back(std::move(image));
        } else {
            ++left_out;
        }
    }
    Json value = std::move(result.value);
    if (left_out > 0) {
        if (!value.is_object()) {
            value = Json{{"result", std::move(value)}};
        }
        value["note_to_assistant"] = std::to_string(left_out) +
                                     " picture(s) of this result could not be passed on to you (too large, too many, or not PNG or "
                                     "JPEG). Ask for a smaller one if the command can make it smaller.";
    }
    reply.text = ai::to_json_text(value);
    return reply;
}

ChatToolReply find_commands(const ai::CommandRegistry& registry, const Json& input) {
    if (!input.contains("words") || !input["words"].is_string()) {
        return {false, "find_commands needs `words`: what to look for"};
    }
    const std::vector<std::string> words = words_of(input["words"].get<std::string>(), true);
    if (words.empty()) {
        return {false, "`words` holds nothing to look for (words of three letters or more, other than \"the\", \"and\" and the like)"};
    }
    usize limit = 20;
    if (input.contains("limit") && input["limit"].is_number()) {
        limit = static_cast<usize>(std::clamp(input["limit"].get<double>(), 1.0, static_cast<double>(k_chat_most_found)));
    }
    // A word asked for is found where a word of the command starts with it
    // ("rain" in "rain" and "rainfall", not in "terrain"). One in a command's
    // name counts for more than one in its description.
    struct Found {
        int score = 0;
        usize index = 0;
    };
    std::vector<Found> found;
    const auto& commands = registry.commands();
    const auto starts = [](const std::vector<std::string>& in, const std::string& word) {
        return std::any_of(in.begin(), in.end(), [&word](const std::string& w) { return w.starts_with(word); });
    };
    usize searched = 0;
    for (usize i = 0; i < commands.size(); ++i) {
        if (not_for_claude(commands[i].name)) {
            continue;
        }
        ++searched;
        const std::vector<std::string> name = words_of(commands[i].name, false);
        const std::vector<std::string> description = words_of(commands[i].description, false);
        int score = 0;
        for (const std::string& word : words) {
            if (starts(name, word)) {
                score += 4;
            } else if (starts(description, word)) {
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
    return {true, ai::to_json_text(Json{{"commands", std::move(list)}, {"matched", found.size()}, {"of", searched}})};
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

ChatToolReply run_command(ai::CommandRegistry& registry, const Json& input, ChatGate* gate) {
    if (!input.contains("name") || !input["name"].is_string()) {
        return {false, "run_command needs `name`"};
    }
    const std::string name = input["name"].get<std::string>();
    if (name == "quit_editor") {
        return {false, "closing the editor is the person's to do"};
    }
    const ai::CommandDesc* command = not_for_claude(name) ? nullptr : registry.find(name);
    if (command == nullptr) {
        return {false, "there is no command named \"" + name + "\" (find_commands searches them)"};
    }
    Json args = Json::object();
    if (input.contains("args") && !input["args"].is_null()) {
        if (!input["args"].is_object()) {
            return {false, "`args` must be an object: the command's arguments by name"};
        }
        args = input["args"];
    }
    if (gate != nullptr && gate->holds(*command, args)) {
        // (The person is not asked about what could not run: the arguments are checked first, as execute will again.)
        if (const std::string wrong = ai::validate_args(command->input_schema, args); !wrong.empty()) {
            return {false, "invalid arguments: " + wrong};
        }
        gate->hold(*command, std::move(args));
        ChatToolReply reply;
        reply.held = true;
        return reply;
    }
    return execute_for_claude(registry, name, args);
}

} // namespace

void ChatGate::answer(bool allow, bool rest) {
    if (!m_held || m_answered) {
        return;
    }
    m_answered = true;
    m_allowed = allow;
    m_rest = m_rest || (allow && rest);
}

bool ChatGate::holds(const ai::CommandDesc& command, const Json& args) const {
    return asks && !m_rest && !command.read_only && !(asks_itself && asks_itself(command.name, args));
}

void ChatGate::chat_asked(std::string_view what) {
    if (what == "stop" || what == "clear") {
        drop();
    }
    if (what == "send" || what == "stop" || what == "clear") {
        message_over();
    }
}

void ChatGate::chat_updated(bool busy) {
    if (!busy) {
        message_over();
    }
}

Json ChatGate::asking() const {
    if (!m_held || m_answered) {
        return nullptr;
    }
    return {{"command", m_request.command}, {"args", m_request.args}, {"args_hidden", m_request.args_hidden}};
}

int held_chat_tool(ai::CommandRegistry& registry, ChatGate& gate, int what, std::string& reply, std::vector<ai::CommandImage>& images) {
    reply.clear();
    images.clear();
    switch (what) {
    case static_cast<int>(HeldAsk::answered):
        return gate.answered() ? k_chat_tool_done : k_chat_tool_held;
    case static_cast<int>(HeldAsk::finish): {
        ChatToolReply result = finish_held_chat_tool(registry, gate);
        reply = std::move(result.text);
        images = std::move(result.images);
        return result.ok ? k_chat_tool_done : k_chat_tool_failed;
    }
    default:
        gate.drop();
        return k_chat_tool_done;
    }
}

void ChatGate::hold(const ai::CommandDesc& command, Json args) {
    GIANT_ASSERT(!m_held, "the chat's gate holds one command at a time");
    const std::string text = ai::to_json_text(args, 2);
    m_request.serial = ++m_serial;
    m_request.command = command.name;
    m_request.about = start_of(command.description);
    m_request.args = utf8_start(text, k_most_shown);
    m_request.args_hidden = text.size() - m_request.args.size();
    m_args = std::move(args);
    m_held = true;
    m_answered = false;
    m_allowed = false;
}

ChatToolReply finish_held_chat_tool(ai::CommandRegistry& registry, ChatGate& gate) {
    if (!gate.m_held) {
        return {false, "not run: the editor no longer holds this command"};
    }
    if (!gate.m_answered) {
        return {false, "not run: the person has not answered yet"};
    }
    gate.m_held = false;
    const Json args = std::move(gate.m_args);
    gate.m_args = Json();
    if (!gate.m_allowed) {
        return {false, "not run: the person was asked and did not allow this. Do not try to get the same done another way: "
                       "tell them what you wanted to do and why, and ask what they would like instead."};
    }
    return execute_for_claude(registry, gate.m_request.command, args);
}

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
           "error. Changes to the scene can be undone with the `undo` command; changes to files cannot. A picture a "
           "command returns (capture_screenshot) comes back for you to look at."},
          {"input_schema",
           {{"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"description", "The command's name."}}},
              {"args", {{"type", "object"}, {"description", "The command's arguments by name, as describe_command's schema says. Leave out for none."}}}}},
            {"required", Json::array({"name"})}}}}});
}

ChatToolReply run_chat_tool(ai::CommandRegistry& registry, std::string_view name, const Json& input, ChatGate* gate) {
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
        return run_command(registry, input, gate);
    }
    return {false, "there is no tool named \"" + std::string(name) + "\""};
}

namespace {

// A field of get_chat's reply, or `otherwise` when it is missing or of another type (never a throw).
std::string text_of(const Json& in, const char* key) {
    const auto found = in.find(key);
    return found != in.end() && found->is_string() ? found->get<std::string>() : std::string();
}
u64 number_of(const Json& in, const char* key, u64 otherwise) {
    const auto found = in.find(key);
    if (found == in.end() || !found->is_number_integer()) { // (signed or not)
        return otherwise;
    }
    return found->is_number_unsigned() ? found->get<u64>() : static_cast<u64>(std::max<long long>(found->get<long long>(), 0));
}
bool flag_of(const Json& in, const char* key, bool otherwise) {
    const auto found = in.find(key);
    return found != in.end() && found->is_boolean() ? found->get<bool>() : otherwise;
}

} // namespace

bool ChatView::update(ai::CommandRegistry& commands, const EditorState& state) {
    if (state.chat_stamp == m_seen) {
        return false;
    }
    m_seen = state.chat_stamp;
    bool changed = false;
    // (A few reads at most: the rest the next time it is drawn.)
    for (int read = 0; read < 8; ++read) {
        const CommandResult reply = commands.execute("get_chat", {{"after", m_after}, {"limit", 256}});
        if (!reply.ok || !reply.value.is_object()) {
            if (read > 0) {
                m_seen = ~u64{0}; // (part way through: the rest at the next draw)
            }
            break;
        }
        const Json& v = reply.value;
        m_started = flag_of(v, "started", false);
        m_key_found = flag_of(v, "key_found", false);
        m_model = text_of(v, "model");
        m_input_tokens = number_of(v, "input_tokens", 0);
        m_output_tokens = number_of(v, "output_tokens", 0);
        // Lines the conversation no longer keeps (it was cleared, or they are too old) go here too.
        const u64 oldest = number_of(v, "oldest", 0);
        const auto kept = std::find_if(m_lines.begin(), m_lines.end(), [oldest](const Line& l) { return l.serial >= oldest; });
        if (kept != m_lines.begin()) {
            m_lines.erase(m_lines.begin(), kept);
            changed = true;
        }
        usize added = 0;
        if (v.contains("entries") && v["entries"].is_array()) {
            for (const Json& e : v["entries"]) {
                if (!e.is_object()) {
                    continue;
                }
                Line line;
                line.serial = number_of(e, "serial", 0);
                line.kind = text_of(e, "kind");
                line.text = text_of(e, "text");
                line.tool = text_of(e, "tool");
                line.ok = flag_of(e, "ok", true);
                m_lines.push_back(std::move(line));
                ++added;
            }
        }
        changed = changed || added > 0;
        const u64 after = number_of(v, "after", m_after);
        const bool more = added == 256 && after > m_after;
        m_after = after;
        if (!more) {
            break;
        }
        if (read == 7) {
            m_seen = ~u64{0}; // (more to read: again at the next draw)
        }
    }
    if (m_lines.size() > k_most_lines) {
        m_lines.erase(m_lines.begin(), m_lines.begin() + static_cast<std::ptrdiff_t>(m_lines.size() - k_most_lines));
    }
    return changed;
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
         "or the key file this command's error names when there is none; and the .NET runtime.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["text"],
             "properties": {"text": {"type": "string", "minLength": 1, "maxLength": 20000,
                                     "description": "What to say to Claude."}}})"),
         false, [ask](const Json& args, CommandContext&) { return ask("send", args); }});

    add(registry,
        {"get_chat",
         "Reads the chat with Claude: the lines after serial `after`, oldest first. A line's `kind` is \"you\" (the "
         "person), \"claude\" (its words), \"tool\" (a tool it called: `tool` names it, `text` is the arguments), "
         "\"result\" (what came back; `ok`), \"note\" or \"error\". Pass the reply's `after` as the next call's to "
         "read on. Also: `busy` (Claude is working), `latest` and `oldest` (the newest line's serial and the oldest "
         "kept; 2,000 are), `started` (false until the first message is sent), and once started `key_found`, "
         "`model` and the tokens of the requests answered so far. `asking`: the command of Claude's the person is being "
         "asked about in the editor's window before it may run ({command, args: its arguments as text, args_hidden: bytes "
         "of them too long to show}), or null; only the person answers, there (no command does), and Claude waits (`busy`) until then.",
         Json::parse(R"({"type": "object", "additionalProperties": false,
             "properties": {"after": {"type": "integer", "minimum": 0, "maximum": 9007199254740991,
                                      "description": "Read the lines after this serial (default 0: from the oldest kept)."},
                            "limit": {"type": "integer", "minimum": 1, "maximum": 256,
                                      "description": "The most lines to return (default 64)."}}})"),
         true, [ask](const Json& args, CommandContext&) { return ask("read", args); }});

    add(registry,
        {"stop_chat",
         "Stops Claude's work on the last chat message: no further commands are run (one running now finishes). "
         "What it already did stays (undo takes it back). A command the person is being asked about is not run, and the "
         "question is withdrawn. The work ends a moment later: `busy` in get_chat says when.",
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
