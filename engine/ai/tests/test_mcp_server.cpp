#include "giant/ai/mcp_server.h"

#include "giant_test.h"

using namespace giant::ai;

namespace {

struct Fixture {
    CommandRegistry registry;
    McpServer server{registry, {"giant-test", "1.2.3", "Use the tools."}};
    double brightness = 1.0;

    Fixture() {
        GIANT_CHECK(registry.add(
            {"set_brightness", "Sets the brightness.",
             Json::parse(R"({"type": "object", "required": ["value"], "additionalProperties": false,
                 "properties": {"value": {"type": "number", "minimum": 0, "maximum": 4}}})"),
             false, [this](const Json& args, CommandContext& context) {
                 const double before = brightness;
                 const double after = args["value"].get<double>();
                 brightness = after;
                 context.record({[this, before] { brightness = before; },
                                 [this, after] { brightness = after; }});
                 return CommandResult::success({{"brightness", brightness}});
             }}));
    }

    // Sends one message and parses the reply (null when there is none).
    Json call(const std::string& message) {
        const auto reply = server.handle(message);
        if (!reply) {
            return Json();
        }
        GIANT_CHECK(reply->find('\n') == std::string::npos); // one line per message
        return parse_json(*reply).value_or(Json());
    }
};

} // namespace

GIANT_TEST("mcp: initialize reports the server and its tools capability") {
    Fixture f;
    const Json reply = f.call(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                   "clientInfo": {"name": "test", "version": "1"}}})");
    GIANT_CHECK(reply["id"] == 1);
    GIANT_CHECK(reply["result"]["protocolVersion"] == "2025-03-26");
    GIANT_CHECK(reply["result"]["serverInfo"]["name"] == "giant-test");
    GIANT_CHECK(reply["result"]["capabilities"].contains("tools"));
    GIANT_CHECK(reply["result"]["instructions"] == "Use the tools.");
}

GIANT_TEST("mcp: an unknown protocol version gets the newest one") {
    Fixture f;
    const Json reply = f.call(R"({"jsonrpc": "2.0", "id": "a", "method": "initialize",
        "params": {"protocolVersion": "1999-01-01"}})");
    GIANT_CHECK(reply["id"] == "a");
    GIANT_CHECK(reply["result"]["protocolVersion"] == McpServer::protocol_versions().front());
}

GIANT_TEST("mcp: notifications and responses get no reply") {
    Fixture f;
    GIANT_CHECK(!f.server.handle(R"({"jsonrpc": "2.0", "method": "notifications/initialized"})"));
    GIANT_CHECK(!f.server.handle(R"({"jsonrpc": "2.0", "id": 4, "result": {}})"));
}

GIANT_TEST("mcp: tools/list lists every command with its schema") {
    Fixture f;
    const Json reply = f.call(R"({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})");
    const Json& tools = reply["result"]["tools"];
    GIANT_CHECK(tools.is_array());
    bool found = false;
    for (const Json& tool : tools) {
        if (tool["name"] == "set_brightness") {
            found = true;
            GIANT_CHECK(tool["inputSchema"]["required"][0] == "value");
            GIANT_CHECK(tool["annotations"]["readOnlyHint"] == false);
        }
        if (tool["name"] == "get_history") {
            GIANT_CHECK(tool["annotations"]["readOnlyHint"] == true);
        }
    }
    GIANT_CHECK(found);
    GIANT_CHECK(tools.size() == f.registry.commands().size());
}

GIANT_TEST("mcp: tools/call runs the command, and undo works through it") {
    Fixture f;
    const Json reply = f.call(R"({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
        "params": {"name": "set_brightness", "arguments": {"value": 2.5}}})");
    GIANT_CHECK(reply["result"]["isError"] == false);
    GIANT_CHECK(reply["result"]["structuredContent"]["brightness"] == 2.5);
    GIANT_CHECK(reply["result"]["content"][0]["type"] == "text");
    GIANT_CHECK(f.brightness == 2.5);

    const Json undo = f.call(R"({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
        "params": {"name": "undo"}})");
    GIANT_CHECK(undo["result"]["isError"] == false);
    GIANT_CHECK(f.brightness == 1.0);
}

GIANT_TEST("mcp: bad tool arguments come back as a tool error the AI can read") {
    Fixture f;
    const Json reply = f.call(R"({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
        "params": {"name": "set_brightness", "arguments": {"value": 9}}})");
    GIANT_CHECK(reply["result"]["isError"] == true);
    const std::string text = reply["result"]["content"][0]["text"].get<std::string>();
    GIANT_CHECK(text.find("value: above the maximum") != std::string::npos);
    GIANT_CHECK(f.brightness == 1.0);
}

GIANT_TEST("mcp: protocol errors use JSON-RPC error codes") {
    Fixture f;
    GIANT_CHECK(f.call("{not json")["error"]["code"] == -32700);
    GIANT_CHECK(f.call("[1, 2]")["error"]["code"] == -32600);
    GIANT_CHECK(f.call(R"({"id": 1, "method": "ping"})")["error"]["code"] == -32600);
    GIANT_CHECK(f.call(R"({"jsonrpc": "2.0", "id": 1, "method": 7})")["error"]["code"] == -32600);
    GIANT_CHECK(f.call(R"({"jsonrpc": "2.0", "id": [1], "method": "ping"})")["error"]["code"] ==
                -32600);
    GIANT_CHECK(f.call(R"({"jsonrpc": "2.0", "id": 1, "method": "nope"})")["error"]["code"] ==
                -32601);
    GIANT_CHECK(f.call(R"({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
        "params": {"name": "missing"}})")["error"]["code"] == -32602);
    GIANT_CHECK(f.call(R"({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": 3})")
                    ["error"]["code"] == -32602);
    const Json ping = f.call(R"({"jsonrpc": "2.0", "id": 9, "method": "ping"})");
    GIANT_CHECK(ping["id"] == 9 && ping["result"].is_object());
}
