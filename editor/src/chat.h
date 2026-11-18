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
#include <vector>

namespace giant::editor {

struct EditorState;

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

// The Chat panel's copy of the conversation: read again (get_chat) only
// when EditorState::chat_stamp says it may have changed, so a frame in
// which nothing happened costs a comparison.
class ChatView {
public:
    struct Line {
        u64 serial = 0;
        std::string kind; // "you", "claude", "tool", "result", "note", "error"
        std::string text;
        std::string tool; // the tool's name, for "tool" and "result"
        bool ok = true;   // "result": whether the tool succeeded
    };
    // The most lines kept (the newest).
    static constexpr usize k_most_lines = 2000;

    // Reads what is new, if anything may be. True when lines were added or
    // dropped (the panel then scrolls to the end).
    bool update(ai::CommandRegistry& commands, const EditorState& state);

    const std::vector<Line>& lines() const { return m_lines; }
    bool started() const { return m_started; }     // a message was ever sent (the chat's C# side runs)
    bool key_found() const { return m_key_found; } // (false until started)
    const std::string& model() const { return m_model; }
    u64 input_tokens() const { return m_input_tokens; }
    u64 output_tokens() const { return m_output_tokens; }

private:
    std::vector<Line> m_lines;
    u64 m_after = 0;
    u64 m_seen = ~u64{0}; // the stamp read at (none yet)
    bool m_started = false;
    bool m_key_found = false;
    std::string m_model;
    u64 m_input_tokens = 0;
    u64 m_output_tokens = 0;
};

// The most commands find_commands answers with at once.
inline constexpr usize k_chat_most_found = 50;

} // namespace giant::editor
