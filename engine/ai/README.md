# ai

The command layer every editor action goes through, the MCP server that lets
Claude Code and the built-in assistant drive the engine, and (later) the rule
checks that keep AI-written code clean.

- `commands.h`: `CommandRegistry`. Commands are named, take JSON arguments
  checked against a JSON Schema, and record undo/redo for what they change.
- `mcp_server.h`: turns MCP JSON-RPC messages into command calls. Every
  command is an MCP tool.
- `json.h`: the JSON type and the non-throwing helpers the module uses.

How to connect Claude Code is in `docs/ai-control.md`.
