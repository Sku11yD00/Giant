// A Model Context Protocol server over the command registry: every command is
// an MCP tool, so Claude Code (or any MCP client) can drive the editor.
//
// The server only turns one JSON-RPC message into one reply; the editor moves
// the text (stdio today). It runs commands, so call it on the registry's
// thread.
#pragma once

#include "giant/ai/commands.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::ai {

struct McpServerInfo {
    std::string name = "giant-editor";
    std::string version = "0.1.0";
    // Shown to the AI when it connects: how to use this server well.
    std::string instructions;
};

class McpServer {
public:
    McpServer(CommandRegistry& registry, McpServerInfo info);

    // Handles one message. Returns the reply text (one line of JSON), or
    // nothing for notifications and responses.
    std::optional<std::string> handle(std::string_view message);

    // The protocol versions this server speaks, newest first.
    static const std::vector<std::string>& protocol_versions();

private:
    Json handle_request(const std::string& method, const Json& params, const Json& id);

    CommandRegistry& m_registry;
    McpServerInfo m_info;
};

} // namespace giant::ai
