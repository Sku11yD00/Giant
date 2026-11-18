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
// The person may have Claude ask first (ChatGate): a command of Claude's
// that changes something then waits until they have answered in the
// editor's window.
//
// Editor only: a game holds no key and talks to no one.
#pragma once

#include "giant/ai/commands.h"

#include <functional>
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
    std::string text;  // the result as JSON, or what went wrong
    bool held = false; // not run yet: the person is asked first (ChatGate); finish_held_chat_tool gives the reply
};

// The person's say over what Claude changes. While `asks`, a command Claude
// runs that changes something (one not read-only) is held, and the person is
// asked in the editor's window (the panels'): they allow it, allow it and
// all else of this message, or do not. The answer is theirs alone, given
// there and on purpose by no command, and no command sets `asks`: a command
// may come from Claude itself, an MCP client or a script.
class ChatGate {
public:
    // What the person is asked about.
    struct Request {
        u64 serial = 0;        // another for each question
        std::string command;
        std::string about;     // what the command is for (its description's start)
        std::string args;      // its arguments as JSON text: the start, when they are long
        usize args_hidden = 0; // how many bytes of them `args` leaves out
    };
    // The most bytes of a command's arguments shown to the person.
    static constexpr usize k_most_shown = 16000;

    // The person's setting (the Chat panel's box). Off where there is no
    // window to ask in.
    bool asks = false;
    // Whether running this command with these arguments (as Claude gave
    // them: not yet checked) would itself only ask the person, in a window
    // of its own, and do nothing more: the gate then does not ask first.
    // load_scripts sets it for a file not yet allowed. Empty: none does.
    std::function<bool(std::string_view command, const ai::Json& args)> asks_itself;

    // The question put to the person now, if any (until finish or drop).
    const Request* request() const { return m_held ? &m_request : nullptr; }
    // Whether it has been answered (true too when there is none).
    bool answered() const { return !m_held || m_answered; }
    // The person's answer to request(). `rest`: with `allow`, whatever else
    // Claude does for this message is allowed too. Nothing when there is no
    // question, or it was answered.
    void answer(bool allow, bool rest = false);
    // The question is withdrawn (Claude's work was stopped or cleared).
    void drop() { m_held = false; }
    // The message Claude worked on is done with (answered, stopped or
    // cleared, or another was sent): "all else of this message" ends.
    void message_over() { m_rest = false; }
    // What the chat's bridge tells: the conversation took a "send", "stop"
    // or "clear" (a new message ends what was allowed for the last; stopped
    // or cleared work takes its question with it, at once)...
    void chat_asked(std::string_view what);
    // ...and each frame, whether Claude is working (when not, the message is over).
    void chat_updated(bool busy);
    // get_chat's `asking`: the command the person is being asked about now
    // ({command, args, args_hidden}), or null.
    ai::Json asking() const;

    // run_chat_tool's: whether this command of Claude's waits for the person.
    bool holds(const ai::CommandDesc& command, const ai::Json& args) const;
    // Holds it (none is held).
    void hold(const ai::CommandDesc& command, ai::Json args);

private:
    friend ChatToolReply finish_held_chat_tool(ai::CommandRegistry& registry, ChatGate& gate);
    Request m_request;
    ai::Json m_args;
    bool m_held = false;
    bool m_answered = false;
    bool m_allowed = false;
    bool m_rest = false; // the person allowed all else of this message
    u64 m_serial = 0;
};

// Runs one of chat_tools() for Claude. Main thread, and not from inside a
// command (commands cannot run commands). With a `gate` that asks, a command
// that changes something is held instead (`held`): its arguments checked,
// nothing run.
ChatToolReply run_chat_tool(ai::CommandRegistry& registry, std::string_view name, const ai::Json& input, ChatGate* gate = nullptr);
// The reply for the command the gate holds, once the person has answered:
// runs it if they allowed it, else tells Claude they did not. The gate then
// holds none. (Not answered, or none held: a failure that says so.)
ChatToolReply finish_held_chat_tool(ai::CommandRegistry& registry, ChatGate& gate);

// What the chat's C# side asks of the tool held (ChatApi::held_tool).
enum class HeldAsk : int {
    answered = 0, // has the person answered? k_chat_tool_done when so (or none is held), k_chat_tool_held when not
    finish = 1,   // its reply (finish_held_chat_tool): k_chat_tool_done or k_chat_tool_failed, with `reply`
    drop = 2,     // it is withdrawn: Claude's work was stopped
};
inline constexpr int k_chat_tool_done = 0;
inline constexpr int k_chat_tool_failed = 1;
inline constexpr int k_chat_tool_held = 2;
int held_chat_tool(ai::CommandRegistry& registry, ChatGate& gate, int what, std::string& reply);

// When the Chat panel reads the conversation anew: a number that moves
// whenever the conversation may have changed, and only then (a read must
// not move it, or the panel would read every frame).
class ChatStamp {
public:
    // The chat was asked something ("send", "read", "stop", "clear"),
    // whether or not it worked (a refused message changes what is shown too).
    void asked(std::string_view what) {
        if (what != "read") {
            ++m_value;
        }
    }
    // The C# side's own stamp this frame (the newest line and whether Claude works).
    void updated(int managed) {
        if (managed != m_managed) {
            m_managed = managed;
            ++m_value;
        }
    }
    u64 value() const { return m_value; }

private:
    u64 m_value = 0;
    int m_managed = 0;
};

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
