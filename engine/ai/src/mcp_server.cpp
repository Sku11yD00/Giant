#include "giant/ai/mcp_server.h"

#include <algorithm>

namespace giant::ai {

namespace {

// JSON-RPC 2.0 error codes.
constexpr int k_parse_error = -32700;
constexpr int k_invalid_request = -32600;
constexpr int k_method_not_found = -32601;
constexpr int k_invalid_params = -32602;

Json error_reply(const Json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json result_reply(const Json& id, Json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// MCP request ids are strings or numbers.
bool valid_id(const Json& id) { return id.is_string() || id.is_number(); }

} // namespace

const std::vector<std::string>& McpServer::protocol_versions() {
    static const std::vector<std::string> versions = {"2025-06-18", "2025-03-26", "2024-11-05"};
    return versions;
}

McpServer::McpServer(CommandRegistry& registry, McpServerInfo info)
    : m_registry(registry), m_info(std::move(info)) {}

std::optional<std::string> McpServer::handle(std::string_view message) {
    const std::optional<Json> parsed = parse_json(message);
    if (!parsed) {
        return to_json_text(error_reply(nullptr, k_parse_error, "not valid JSON"));
    }
    const Json& msg = *parsed;
    if (!msg.is_object()) {
        // Batches were removed from MCP in 2025-06-18.
        return to_json_text(error_reply(nullptr, k_invalid_request, "expected one JSON-RPC object"));
    }
    const auto method_it = msg.find("method");
    const auto id_it = msg.find("id");
    if (method_it == msg.end()) {
        if (msg.contains("result") || msg.contains("error")) {
            return std::nullopt; // a response to something we never sent
        }
        return to_json_text(error_reply(nullptr, k_invalid_request, "not a JSON-RPC message"));
    }
    if (!method_it->is_string() || msg.value("jsonrpc", Json()) != "2.0") {
        const Json id = id_it != msg.end() && valid_id(*id_it) ? *id_it : Json();
        return to_json_text(error_reply(id, k_invalid_request, "not a JSON-RPC 2.0 request"));
    }
    if (id_it == msg.end()) {
        return std::nullopt; // a notification, e.g. notifications/initialized
    }
    if (!valid_id(*id_it)) {
        return to_json_text(error_reply(nullptr, k_invalid_request, "id must be a string or integer"));
    }
    static const Json no_params = Json::object();
    const auto params_it = msg.find("params");
    const Json& params = params_it != msg.end() ? *params_it : no_params;
    if (!params.is_object()) {
        return to_json_text(error_reply(*id_it, k_invalid_params, "params must be an object"));
    }
    return to_json_text(handle_request(method_it->get<std::string>(), params, *id_it));
}

Json McpServer::handle_request(const std::string& method, const Json& params, const Json& id) {
    if (method == "initialize") {
        const auto& versions = protocol_versions();
        std::string version = versions.front();
        if (auto it = params.find("protocolVersion"); it != params.end() && it->is_string()) {
            const std::string asked = it->get<std::string>();
            if (std::find(versions.begin(), versions.end(), asked) != versions.end()) {
                version = asked;
            }
        }
        Json result = {{"protocolVersion", version},
                       {"capabilities", {{"tools", {{"listChanged", false}}}}},
                       {"serverInfo", {{"name", m_info.name}, {"version", m_info.version}}}};
        if (!m_info.instructions.empty()) {
            result["instructions"] = m_info.instructions;
        }
        return result_reply(id, std::move(result));
    }
    if (method == "ping") {
        return result_reply(id, Json::object());
    }
    if (method == "tools/list") {
        Json tools = Json::array();
        for (const auto& command : m_registry.commands()) {
            tools.push_back({{"name", command.name},
                             {"description", command.description},
                             {"inputSchema", command.input_schema},
                             {"annotations", {{"readOnlyHint", command.read_only}}}});
        }
        return result_reply(id, {{"tools", std::move(tools)}});
    }
    if (method == "tools/call") {
        const auto name = params.find("name");
        if (name == params.end() || !name->is_string()) {
            return error_reply(id, k_invalid_params, "tools/call needs a tool name");
        }
        if (m_registry.find(name->get<std::string>()) == nullptr) {
            return error_reply(id, k_invalid_params, "unknown tool: " + name->get<std::string>());
        }
        static const Json no_args;
        const auto args_it = params.find("arguments");
        const Json& args = args_it != params.end() ? *args_it : no_args;
        const CommandResult outcome = m_registry.execute(name->get<std::string>(), args);
        if (!outcome.ok) {
            // A tool failure is a normal result the AI reads and can react to.
            return result_reply(id, {{"content", {{{"type", "text"}, {"text", outcome.error}}}},
                                     {"isError", true}});
        }
        Json result = {{"content", {{{"type", "text"}, {"text", to_json_text(outcome.value, 2)}}}},
                       {"isError", false}};
        if (outcome.value.is_object()) {
            result["structuredContent"] = outcome.value;
        }
        return result_reply(id, std::move(result));
    }
    return error_reply(id, k_method_not_found, "unknown method: " + method);
}

} // namespace giant::ai
