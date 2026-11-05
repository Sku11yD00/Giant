# Driving the editor with AI

Everything the editor can do is a command (AGENTS.md rule 7). The same
commands are available to people (menus and panels), to C# scripts, to the
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
GPU is the editor using?" or "build a small village with a church and light it
like late afternoon"; Claude calls the tools, looks at the result with
`capture_screenshot`, and can undo what it did.

## Commands so far

| Command | Changes things | What it does |
| --- | --- | --- |
| `get_engine_info` | no | Version, graphics backend and GPU, validation layer, frames, frame times over the last 240 frames (average, median, 99th percentile, worst, hitches: frames 1.5x the median and 2 ms over it, and `recent_ms`, the times oldest first, with `recorded`, the count ever; wall clock), `gpu`: the GPU's own time for the newest finished frame (`frame_ms`) and per pass (`passes`: `name` and `ms`, from GPU timestamps; passes run twice, such as culling, are added up; 0 ms on the null backend), window size |
| `get_log` | no | Newest log lines, filtered by level |
| `create_entity`, `create_entities` | yes, undoable | Adds shapes (cube, sphere, plane), imported meshes (`mesh`: a glTF/GLB path), kit placements (`kit`: a kit's name) or decals (`shape: decal` with `decal` settings: stamp shape `disc`, `rectangle`, `ring`, `splat` or `cracks`, opacity, softness, seed) with position, rotation, scale and material; many in one step |
| `create_kit` | yes, undoable | Makes a kit, a reusable group of pieces, from `pieces` or `from_entities` (`replace_entities` swaps those for one placement); `replace` changes an existing kit and every placement |
| `delete_kit` | yes, undoable | Removes a kit nothing places |
| `list_kits`, `get_kit` | no | Kits with piece and placement counts; one kit's pieces |
| `set_entity` | yes, undoable | Changes any fields of one entity |
| `delete_entities` | yes, undoable | Removes entities by id |
| `get_entity`, `list_entities` | no | Reads entities (outliner order) |
| `get_lighting`, `set_lighting` | `set` is undoable | Sun height and direction, sun, sky and ground light, exposure |
| `get_clear_color`, `set_clear_color` | `set` is undoable | The sky colour, which is also the background |
| `new_scene`, `load_scene` | yes, undoable | Empties the scene, or loads a `.scene.json` file |
| `save_scene` | writes a file | Saves the scene as JSON text (meshes by path) |
| `import_mesh` | loads an asset, not undoable | Imports a `.gltf`/`.glb` and builds its cluster LOD; `reload` picks up changes |
| `list_meshes` | no | Imported meshes with triangle, cluster and level counts |
| `get_render_settings`, `set_render_settings` | view only, not undoable | LOD error in pixels, debug view (`lit`, `clusters`, `lod_levels`, `triangles`, `voxels`), `gpu_culling`, `occlusion_culling`, `mesh_shaders`, `antialiasing` (SMAA edges), `specular_antialiasing`, `voxel_world` and `voxel_ms_per_frame` (keeping the voxel copy of the scene built, and its CPU time per frame); last frame's `draws` and `post_draws` (full-screen anti-aliasing passes), cluster and triangle counts (from the GPU a frame or two late when `from_gpu`), occluded counts, `overflow`, `materials` (shared table size), `decals` (in view) and `decal_overflow`, `streaming` (pool size and use, groups loaded, loaded and waiting this frame) and `voxels` (bricks in the pool, its capacity, `pending` work still to do, `built` this frame, `missing` bricks that did not fit and `overflow`, `cpu_ms`) |
| `probe_voxels` | no | What the voxel world holds at a world `position`, per clipmap level finest first (`level`, `voxel_size`, `state`: `outside` the level's window, `building` (the next coarser level stands in), `empty` or `solid`); settled once `voxels.pending` is 0 |
| `get_camera`, `set_camera` | view only, not undoable | The viewport camera; `look_at` aims it |
| `set_window_size` | view only, not undoable | Resizes the window (screen points) |
| `undo`, `redo` | yes | Walks the change history |
| `get_history` | no | What can be undone and redone |
| `capture_screenshot` | no | A PNG of the editor as it looks now, so the AI can see it |
| `load_scripts`, `unload_scripts` | runtime only | Loads, reloads or stops C# scripts (`docs/scripting.md`) |
| `get_scripts` | no | The loaded script assembly and how many scripts run |
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
