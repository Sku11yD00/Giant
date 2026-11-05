# Driving the editor with AI

Everything the editor can do is a command (AGENTS.md rule 7). The same
commands are available to people (menus and panels, from part 3b), to the
built-in assistant (later), and to outside AI tools through MCP.

## Connect Claude Code

The editor is an MCP server on stdin/stdout when started with `--mcp`.
Claude Code starts it for you. On Windows, from the folder that holds
`giant_editor.exe`:

```
claude mcp add giant -- giant_editor.exe --mcp
```

Or put this in a `.mcp.json` next to your project (use the full path to the
exe):

```json
{
  "mcpServers": {
    "giant": { "command": "C:/Giant/giant_editor.exe", "args": ["--mcp"] }
  }
}
```

The editor window opens when Claude Code connects and closes when it
disconnects. Ask things like "make the background a sunset orange" or "what
GPU is the editor using?"; Claude calls the tools and can undo what it did.

## Commands so far

| Command | Changes things | What it does |
| --- | --- | --- |
| `get_engine_info` | no | Version, graphics backend and GPU, validation layer, frames, window size |
| `get_log` | no | Newest log lines, filtered by level |
| `get_clear_color` | no | Viewport background colour (linear RGB) |
| `set_clear_color` | yes, undoable | Sets the background colour |
| `set_window_size` | view only, not undoable | Resizes the window (screen points) |
| `undo`, `redo` | yes | Walks the change history |
| `get_history` | no | What can be undone and redone |
| `quit_editor` | closes | Closes the editor |

## Adding a command

1. Write it next to the feature, with a JSON Schema for its arguments
   (`minimum`, `maximum`, `enum` and `required` keep AI calls honest) and a
   description written for the AI: what it does and anything surprising.
2. If it changes state, record a `Change` with undo and redo. If it fails
   halfway, return `CommandResult::failure`; recorded changes roll back.
3. Mark read-only commands `read_only`; they may not record changes. View
   state (window size, camera, panel layout) is not undoable, the same as
   when a person changes it by hand.
4. Test it through `CommandRegistry::execute`, and add it to
   `tools/mcp_smoke.py` if it is part of a workflow the AI will use.
