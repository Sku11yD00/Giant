# Giant engine

The Giant engine is a fork of [Godot](https://godotengine.org) for realistic open-world games
about giants that grow from human size to kilometres tall. It tracks Godot's 4.8 development
branch and adds the systems the game needs on top.

## What is different from Godot

- **Large worlds by default.** Builds always use `precision=double`.
- **No TAA smear.** Projects default to 4x MSAA and SMAA with TAA off, and every Giant feature
  must look clean that way.
- **AI control built in.** The editor runs an MCP server, so Claude Code, the in-editor assistant
  or any other MCP client can drive the whole engine.

## Connecting Claude Code (or another AI tool)

When the editor opens a project it starts the MCP server and prints its address in the Output
panel, for example `Giant MCP server listening on http://127.0.0.1:7400/mcp`.

The address and access token are also written to `.godot/giant_mcp.json` inside the project.
That folder is never committed, so the token stays on your PC.

The Output panel also prints the full command with your token filled in. Copy that line and
run it from the project folder to add the server to Claude Code:

```
claude mcp add --transport http giant http://127.0.0.1:7400/mcp --header "Authorization: Bearer <your token>"
```

Settings live under Editor Settings > Giant MCP (turn on Advanced Settings to see them): turn
the server on or off, change the port, or replace the token. `GIANT_MCP_PORT` and `GIANT_MCP_TOKEN` environment variables override them.

### What an AI can do

| Tool | What it does |
| --- | --- |
| `engine_info` | Version, project, open scenes and anti-aliasing settings |
| `get_scene_tree`, `get_node` | Read the open scene and any node's properties |
| `list_classes`, `describe_class` | Search and explain any engine class from the reflection data |
| `create_node`, `set_properties`, `delete_node` | Edit the scene; every change can be undone |
| `call_method` | Call any method on a node |
| `new_scene`, `open_scene`, `save_scene` | Manage scene files |
| `list_files`, `read_file`, `write_file`, `attach_script` | Work with scripts and other project files; GDScript errors come back at once |
| `play`, `stop`, `get_log` | Run the game and read its output |
| `undo`, `redo` | Step through the scene's undo history |
| `screenshot` | See the 3D or 2D viewport, or the whole editor |

### Safety

- The server only listens on 127.0.0.1 and refuses requests from web pages.
- Every request needs the bearer token.
- Scene edits go through the editor's undo history.
- Tools that delete or run arbitrary code are marked destructive, so AI clients ask before using them.

## Updating to newer Godot versions

This repository started from a single import of Godot's source, without upstream history.
`giant/UPSTREAM` records the upstream commit it matches. To move to a newer Godot commit,
apply the upstream changes between the recorded commit and the new one as a patch:

```
git clone --filter=blob:none https://github.com/godotengine/godot /tmp/godot-upstream
git -C /tmp/godot-upstream diff --binary $(cat giant/UPSTREAM) <new commit> > /tmp/upstream.patch
git apply --3way /tmp/upstream.patch
echo <new commit> > giant/UPSTREAM
```

Resolve any conflicts in files marked `// Giant engine:`, run the tests, and commit the update
as one commit named after the new upstream commit.
