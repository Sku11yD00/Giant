// The editor's chat with Claude (living world part LW6b): the person writes
// what they want in plain words, and Claude does it with the editor's own
// commands, the ones their buttons run (rule 7), so what it does is
// recorded in the undo history like anything else.
//
// The conversation itself (what is sent to Claude's service, and the key)
// is C#, in scripts/Giant.Chat, reached through ChatLink. Here are the
// tools Claude is given, which need no .NET:
//
//   find_commands     search the command registry by words
//   describe_command  a command's description and argument schema
//   run_command       run one, as the person would
//
// Editor only: a game holds no key and talks to no one.
#pragma once

#include "giant/ai/commands.h"

#include <string>
#include <string_view>

namespace giant::editor {

// The conversation's side (ChatBridge in the editor; null in tests and
// tools without .NET).
class ChatLink {
public:
    virtual ~ChatLink() = default;
    // "send" {text}, "read" {after, limit}, "stop", "clear".
    virtual ai::CommandResult ask(std::string_view what, const ai::Json& args) = 0;
};

// The tools Claude is given: an array of {name, description, input_schema}.
ai::Json chat_tools();

struct ChatToolReply {
    bool ok = false;
    std::string text; // the result as JSON, or what went wrong
};
// Runs one of chat_tools() for Claude. Main thread, and not from inside a
// command (commands cannot run commands).
ChatToolReply run_chat_tool(ai::CommandRegistry& registry, std::string_view name, const ai::Json& input);

// The most commands find_commands answers with at once.
inline constexpr usize k_chat_most_found = 50;

} // namespace giant::editor
