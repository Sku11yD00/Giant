#include "giant/ai/commands.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include <algorithm>
#include <exception>

namespace giant::ai {

CommandResult CommandResult::success(Json value) {
    CommandResult result;
    result.value = std::move(value);
    return result;
}

CommandResult CommandResult::failure(std::string error) {
    CommandResult result;
    result.ok = false;
    result.value = Json();
    result.error = std::move(error);
    return result;
}

void CommandContext::record(Change change) {
    GIANT_ASSERT(change.undo && change.redo, "a change needs both undo and redo");
    m_changes.push_back(std::move(change));
}

bool is_valid_command_name(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!letter(name.front())) {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [&](char c) { return letter(c) || digit(c) || c == '_'; });
}

namespace {

Json entries_to_json(const std::vector<HistoryEntry>& entries) {
    Json out = Json::array();
    for (const auto& entry : entries) {
        out.push_back({{"command", entry.command}, {"args", entry.args}});
    }
    return out;
}

void undo_all(std::vector<Change>& changes) {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        it->undo();
    }
}

} // namespace

CommandRegistry::CommandRegistry() {
    const auto ok = [](bool added) { GIANT_ASSERT(added, "built-in command"); };
    ok(add({"undo",
            "Undoes the most recent change made by a command. Fails if there is nothing to undo.",
            Json{{"type", "object"}, {"additionalProperties", false}}, false,
            [this](const Json&, CommandContext&) {
                return undo() ? CommandResult::success({{"undone", true}})
                              : CommandResult::failure("nothing to undo");
            }}));
    ok(add({"redo", "Redoes the change most recently undone. Fails if there is nothing to redo.",
            Json{{"type", "object"}, {"additionalProperties", false}}, false,
            [this](const Json&, CommandContext&) {
                return redo() ? CommandResult::success({{"redone", true}})
                              : CommandResult::failure("nothing to redo");
            }}));
    ok(add({"get_history",
            "Lists the commands that can be undone (oldest first) and those that can be redone.",
            Json{{"type", "object"}, {"additionalProperties", false}}, true,
            [this](const Json&, CommandContext&) {
                return CommandResult::success({{"undo", entries_to_json(undo_history())},
                                               {"redo", entries_to_json(redo_history())}});
            }}));
}

bool CommandRegistry::add(CommandDesc desc) {
    if (m_running) {
        GIANT_ASSERT(false, "commands may not register commands while running");
        return false;
    }
    if (!is_valid_command_name(desc.name)) {
        log_error("ai", "invalid command name: '" + desc.name + "'");
        return false;
    }
    if (find(desc.name) != nullptr) {
        log_error("ai", "command registered twice: " + desc.name);
        return false;
    }
    if (!desc.run) {
        log_error("ai", "command has no run function: " + desc.name);
        return false;
    }
    m_commands.push_back(std::move(desc));
    return true;
}

const CommandDesc* CommandRegistry::find(std::string_view name) const {
    for (const auto& command : m_commands) {
        if (command.name == name) {
            return &command;
        }
    }
    return nullptr;
}

CommandResult CommandRegistry::execute(std::string_view name, const Json& args_in) {
    GIANT_ASSERT(!m_running, "commands may not run other commands");
    if (m_running) {
        return CommandResult::failure("commands may not run other commands");
    }
    const CommandDesc* command = find(name);
    if (command == nullptr) {
        return CommandResult::failure("unknown command: " + std::string(name));
    }
    const Json args = args_in.is_null() ? Json::object() : args_in;
    if (std::string error = validate_args(command->input_schema, args); !error.empty()) {
        return CommandResult::failure("invalid arguments: " + error);
    }

    const bool read_only = command->read_only;
    const std::string command_name = command->name;
    CommandContext context;
    m_running = true;
    CommandResult result;
    try {
        result = command->run(args, context);
    } catch (const std::exception& e) {
        // A safety net: commands should return failures, not throw.
        result = CommandResult::failure("internal error in '" + command_name + "': " + e.what());
    }
    m_running = false;

    if (read_only && !context.changes().empty()) {
        GIANT_ASSERT(false, "a read-only command recorded changes");
        undo_all(context.changes());
        return CommandResult::failure("internal error: read-only command '" + command_name +
                                      "' changed something");
    }
    if (!result.ok) {
        undo_all(context.changes());
        return result;
    }
    if (!context.changes().empty()) {
        m_undo.push_back({{command_name, args}, std::move(context.changes())});
        if (m_undo.size() > k_max_history) {
            m_undo.pop_front();
        }
        m_redo.clear();
    }
    return result;
}

bool CommandRegistry::undo() {
    if (m_undo.empty()) {
        return false;
    }
    Entry entry = std::move(m_undo.back());
    m_undo.pop_back();
    undo_all(entry.changes);
    m_redo.push_back(std::move(entry));
    return true;
}

bool CommandRegistry::redo() {
    if (m_redo.empty()) {
        return false;
    }
    Entry entry = std::move(m_redo.back());
    m_redo.pop_back();
    for (auto& change : entry.changes) {
        change.redo();
    }
    m_undo.push_back(std::move(entry));
    return true;
}

std::vector<HistoryEntry> CommandRegistry::undo_history() const {
    std::vector<HistoryEntry> out;
    for (const auto& entry : m_undo) {
        out.push_back(entry.what);
    }
    return out;
}

std::vector<HistoryEntry> CommandRegistry::redo_history() const {
    std::vector<HistoryEntry> out;
    for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it) {
        out.push_back(it->what);
    }
    return out;
}

} // namespace giant::ai
