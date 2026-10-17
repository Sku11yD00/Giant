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
- **Giants.** Characters that grow smoothly from human size to kilometres tall, with jiggle,
  hair, animation and skin detail that keep working at every size.

## Try the giant prototype

Open `giant/demo` in the Giant editor (in the Windows download it is the `giant-prototype`
folder next to the editor) and press F5. Use the panel to grow her, change the time feel, make
her walk and switch the detail layers off to see what normal textures would do. Keys 1, 2 and
3 switch between orbiting her, walking around on foot, and flying.

## Giants

A giant is an ordinary character placed under a `GiantBody` node. The node's origin must sit
on the ground between her feet.

| Node | What it does |
| --- | --- |
| `GiantBody` | Sets her `height` in metres (the node's scale follows) and how slowly she moves at that size. `grow_to(height, seconds)` grows her smoothly. |
| `GiantJiggle3D` | Jiggle and hair. Add it under the `Skeleton3D` and list the bones in `chains`. |
| `GiantFootsteps3D` | Emits `footstep(foot, position, giant_height)` when a foot lands, for shake, dust and damage. |

**Time feel.** Real physics makes something `s` times bigger move `sqrt(s)` times slower. A
giant that obeys this exactly looks like slow motion; one that ignores it looks like a toy.
`GiantBody.time_feel` blends the two: `0` moves like a human at every size, `1` is fully
realistic, and the default `0.5` sits in between. Animation and jiggle both play at this tempo.

**Jiggle and hair at any size.** `GiantJiggle3D` simulates in the giant's body space (skeleton
space before her scale) and in her own time. The same settings therefore give the same shape at
1.8 m and at 1.5 km, growing never makes anything jump, and the result barely depends on the
frame rate. Unit tests in `modules/giant_core/tests` check all three.

**Skin that never stretches.** Every mesh under a `GiantBody` gets the instance shader
parameter `giant_scale`. A shader can use it to tile fine detail (pores, fabric weave) at a
fixed size in world metres: as she grows, finer copies of the detail texture fade in, so a
close-up of a 1 km giant looks as sharp as one of a person. See
`giant/demo/giant/giant_skin.gdshader`. Give the character UVs at an even texel density in
metres, as is normal for detail maps.

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

The giant nodes are ordinary engine classes, so the same tools create, size and grow giants.

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
