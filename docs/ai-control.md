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
| `save_world` | writes files | Saves the open world (`world.json` and each 64 m cell file whose entities or terrain tile changed; a cell's file holds its tile, which streams with it), or with `folder` and no world open makes a new world there from the scene and opens it |
| `open_world` | yes, undoable | Opens a world folder: the scene becomes the world, cells near the camera load in the background and far ones unload (only when they hold no unsaved edits). Refused while the open world has unsaved edits unless `discard_unsaved` |
| `close_world` | not undoable | Stops streaming; the scene stays as it is, as a plain scene |
| `get_world`, `set_world_streaming` | view only, not undoable | Streaming progress (`open`, `active`: the scene is the world's, `folder`, `cells` with a file, `loaded`, `reading`, `waiting`, `failed`, `unsaved_cells`, `entities`, `entities_loaded`, `entities_unloaded`, `ids_changed`: entities given a new id because theirs was taken, `ground`: cells whose file holds a terrain tile, `tiles_loaded`, `tiles_unloaded`) and `streaming` settings (`load_radius` in metres, default 1536, at most 8192; `unload_margin`, default 128; `entities_per_frame`, default 4096; `reads_in_flight`, default 16; `unload_every` frames, default 30) |
| `create_terrain` | yes, undoable | Ground tiles for every 64 m cell the area `from`-`to` (x, z metres) touches: flat at `height` or rolling `hills` (`base`, `amplitude`, `size`, `octaves`, `seed`); `samples` per tile side (8 to 128, default 64: 1 m apart) changes only with no terrain (in a streamed world whose files hold tiles, it is theirs); at most 4096 tiles a command |
| `remove_terrain` | yes, undoable | Removes the tiles an area touches, or `all` |
| `set_terrain_heights` | yes, undoable | One tile's `samples` x `samples` heights, rows along +x from the cell's corner, rows going +z |
| `set_terrain_material` | yes, undoable | The ground's `base_color` and `roughness` |
| `get_terrain` | no | Tiles, `samples`, `spacing`, the cells covered, the material, `meshes` (`tiles` drawn, `building`, `built`, `triangles`); `points` gives the ground's height at each [x, z] (null off the terrain); `cell` gives a tile's heights |
| `import_mesh` | loads an asset, not undoable | Imports a `.gltf`/`.glb` and builds its cluster LOD; `reload` picks up changes |
| `list_meshes` | no | Imported meshes with triangle, cluster and level counts |
| `get_render_settings`, `set_render_settings` | view only, not undoable | LOD error in pixels, debug view (`lit`, `clusters`, `lod_levels`, `triangles`, `voxels`, `lighting`: the lit view in plain white, the light alone), `gpu_culling`, `occlusion_culling`, `mesh_shaders`, `antialiasing` (SMAA edges), `specular_antialiasing`, `voxel_world` and `voxel_ms_per_frame` (keeping the voxel copy of the scene built, and its CPU time per frame), `voxel_far_field`, `far_field_distance` (metres, default 3000) and `far_field_max_radius` (metres, default 50: past the distance, instances that small are left to the voxel world), `shadows` (sun shadows, default on: lit view with GPU culling) and `shadow_pages_per_frame` (default 64), `gi` (global illumination from the lit voxel world, default on: lit views and the voxel view, with `voxel_world`) and `gi_bricks_per_frame` (voxel bricks lit a frame, default 256), `reflections` (glossy surfaces reflect the lit voxel world, default on: lit views, needs `gi`) and `screen_space_reflections` (sharp reflections from the image itself where what they meet is on screen, default on); last frame's `draws` and `post_draws` (full-screen passes after shading: screen reflections with tone mapping when on, then anti-aliasing), cluster and triangle counts (from the GPU a frame or two late when `from_gpu`), occluded counts, `overflow`, `materials` (shared table size), `decals` (in view) and `decal_overflow`, `cells` (`count` of world cells handed over, holes left by cells that went included, how many `changed`: compared for changes this frame), `streaming` (pool size and use, groups loaded, loaded and waiting this frame) and `voxels` (bricks in the pool, its capacity, `pending` work still to do, `built` this frame, `missing` bricks that did not fit and `overflow`, `cpu_ms`) and `far_field` (`on` this frame, `culled` instances left to the voxel world) and `shadows` (`on` this frame, shadow `pages` resident of `capacity`, `drawn` this frame, `waiting` to be drawn, `missing` pages that found no room, raster `draws`) and `gi` (`on` this frame, bricks `lit` and `cleared` this frame, new or rebuilt bricks `waiting` to be lit) and `reflections` (`voxel` cones and `screen`-space rays on this frame) |
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
