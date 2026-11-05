// The command layer. Every editor action is a named command with typed
// arguments (a JSON Schema), so people, the built-in assistant and outside AI
// tools such as Claude Code all drive the engine the same way. Commands that
// change something record how to undo it.
//
// Commands run on the thread that owns the registry (the editor's main
// thread). The registry is not thread-safe.
#pragma once

#include "giant/ai/json.h"
#include "giant/core/types.h"

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::ai {

// A picture a command returns (for example a screenshot), so the AI can see
// it. MCP clients show it as image content.
struct CommandImage {
    std::string mime_type; // "image/png"
    std::string data;      // the encoded file (raw bytes, not base64)
};

struct [[nodiscard]] CommandResult {
    bool ok = true;
    Json value = Json::object(); // the answer, on success
    std::string error;           // what went wrong, on failure
    std::vector<CommandImage> images;

    static CommandResult success(Json value = Json::object());
    static CommandResult failure(std::string error);
};

// One reversible change. `undo` puts things back as they were before the
// change; `redo` makes the change again. Neither may throw: if one does, the
// registry logs it and clears the whole history, since it no longer matches
// the state.
struct Change {
    std::function<void()> undo;
    std::function<void()> redo;
};

class CommandContext {
public:
    // Records a change the command just made. If the command then fails,
    // recorded changes are undone (newest first), so a command either fully
    // happens or not at all.
    void record(Change change);

    std::vector<Change>& changes() { return m_changes; }

private:
    std::vector<Change> m_changes;
};

struct CommandDesc {
    // Letters, digits and underscores, starting with a letter, at most 64
    // characters (what MCP clients accept as a tool name).
    std::string name;
    // One or two sentences for the AI: what it does, and anything surprising.
    std::string description;
    // JSON Schema for the arguments, checked before `run`. Must be an object
    // schema (MCP requires it). Give integer fields a minimum and maximum
    // before converting them to C++ integer types.
    Json input_schema = Json{{"type", "object"}, {"additionalProperties", false}};
    // Changes nothing (safe to call any time). Read-only commands may not
    // record changes.
    bool read_only = false;
    std::function<CommandResult(const Json& args, CommandContext& context)> run;
};

struct HistoryEntry {
    std::string command;
    Json args;
};

class CommandRegistry {
public:
    // Registers the built-in commands: undo, redo and get_history.
    CommandRegistry();

    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    // False (and logged) if the name is invalid or taken, or `run` is empty.
    [[nodiscard]] bool add(CommandDesc desc);

    const std::vector<CommandDesc>& commands() const { return m_commands; }
    const CommandDesc* find(std::string_view name) const;

    // Checks the arguments and runs the command. A missing or null `args`
    // means no arguments. A successful command that recorded changes goes on
    // the undo history and clears the redo history.
    //
    // `merge_group`: consecutive runs of the same command with the same
    // non-zero group become one undo step. UI drags use it (one group per
    // drag), so dragging a colour picker is undone in one go.
    CommandResult execute(std::string_view name, const Json& args = Json::object(),
                          u64 merge_group = 0);

    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    // Undoes or redoes the newest entry. False if there is nothing to do (or
    // the change threw, which clears the history).
    bool undo();
    bool redo();

    // Oldest first: the last entry is the next one `undo` reverts.
    std::vector<HistoryEntry> undo_history() const;
    // Next first: the first entry is the next one `redo` repeats.
    std::vector<HistoryEntry> redo_history() const;

    static constexpr usize k_max_history = 256;

private:
    struct Entry {
        HistoryEntry what;
        std::vector<Change> changes;
        u64 merge_group = 0;
    };

    void drop_history();

    std::vector<CommandDesc> m_commands;
    std::deque<Entry> m_undo; // newest at the back
    std::deque<Entry> m_redo; // next to redo at the back
    bool m_running = false;   // a command is executing (no nesting)
};

// True for names MCP clients accept: [A-Za-z][A-Za-z0-9_]{0,63}.
bool is_valid_command_name(std::string_view name);

} // namespace giant::ai
