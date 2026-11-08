# Architecture

Five layers. Each layer only uses the layers below it.

```
game        C# scripts, levels, materials, scatter graphs, weather presets
editor      panels, play-in-editor, undo; uses everything below
ai          command layer, MCP server, rule checks
gameplay    game rules: items, inventories, weapon parts, crafting, loot (docs/game-plan.md)
sim nav world  giants, destruction, NPCs, animation | walkable ground, paths | cells, streaming, landscape, scatter, weather
render      virtual geometry, voxel world, GI, materials, hair, clouds
rhi geometry voxel  thin graphics layer (D3D12 first) | meshes, import, cluster LOD (CPU only) | voxel bricks, clipmap
platform    windows, input, files (SDL3 underneath)
core        jobs, memory, double-precision math, logging, reflection, text formats
```

## Allowed module dependencies

This table is the source of truth for `tools/check_architecture.py`.

| Module | May include |
| --- | --- |
| core | (nothing) |
| platform | core |
| rhi | core |
| anim | core |
| geometry | core, anim |
| voxel | core |
| render | core, rhi, geometry, voxel |
| world | core, voxel |
| sim | core, world |
| nav | core, world |
| gameplay | core, world, sim |
| script | core, world, sim |
| ai | core, platform, rhi, anim, geometry, render, world, sim, script |
| editor | everything |

`rhi` never includes `platform`: a swap chain takes the native window handle
as plain data, so the graphics layer works headless and in tests.

`render` reads world state through plain data handed to it each frame (cells,
instances, weather values, voxel edits); it never includes `world` or `sim`.
This keeps the renderer replaceable.

## Graphics layer

`rhi` (engine/rhi/include/giant/rhi/rhi.h) is a thin, bindless layer. The
rest of the engine reaches the GPU only through it, so a Vulkan backend can
follow without touching the renderer.

- **Handles.** Buffers, textures and pipelines are `Handle<Tag>{index,
  generation}` into device pools. A destroyed slot bumps its generation, so
  a stale handle stops resolving instead of reaching whatever is created
  there next. Zero is "none".
- **Bindless.** Shader model 6.6: every shader-visible resource has a
  descriptor index in one heap (`ResourceDescriptorHeap` in shaders), and
  draws pass indices in up to 128 bytes of push constants (register b0).
  One global root signature, so pipelines need no binding setup. Four fixed
  samplers. Shaders include `giant/bindless.slang` (engine/rhi/shaders) and
  never touch the heaps directly.
- **Vertex pulling.** No vertex buffers or input layouts: vertex shaders read
  what they need from buffers by `SV_VertexID`. Index buffers stay (32-bit).
- **Frames.** `begin_frame(swap_chain or null)` returns the frame's command
  list; `end_frame` submits (and presents). Two frames in flight live on the
  device. Destroying anything is deferred until the GPU is done with it, so it
  is safe mid-frame. Per-frame transient memory (`allocate`, `upload`) holds
  constants, instance lists, streamed geometry and UI geometry; each slot
  makes 32 MiB of it at init (the default streaming budget), so a streaming
  burst does not create upload buffers mid-frame.
- **Uploads (N2 part 1).** A buffer or texture created with initial data
  inside a frame never waits for the GPU: the data goes into the frame
  slot's staging memory (persistent, mapped upload-heap chunks of 8 MiB,
  four per slot made at init, bump-allocated and reused once that slot's
  fence is reached; a burst past them adds chunks, kept until unused for
  120 frames; buffer data is split across chunks, so only a texture larger
  than a chunk needs one of its own) and is copied by the frame's upload list,
  submitted on its own just before the frame's list on the same queue.
  Outside a frame (load time) the copy is still waited for.
  `FrameStats::upload_bytes` counts what a frame staged; the caller keeps
  it within its budget.
- **Small buffers share heaps.** A buffer that is its own allocation (a
  committed resource) is new video memory: Windows finds, zeroes and maps
  it, and unmaps it when it goes, on the GPU's timeline, and the queue
  waits behind that work. Streaming makes and drops thousands of small
  buffers (terrain tiles, meshes), so on D3D12 a gpu-memory buffer up to
  4 MiB made with all its contents is placed in one of a few 64 MiB heaps
  instead (four made at init, 256 MiB, more when they fill, none given
  back), a range of 64 KiB units each (src/heap_ranges.h; a tiny buffer
  takes a whole unit, as a committed one does). The heaps are not zeroed
  and their ranges are reused, so a buffer made without its contents stays
  committed (it comes zeroed). Larger buffers and textures are committed
  too; if a heap cannot be made, small buffers are committed from then on.
  `Device::counts()` keeps running totals of resources created, new memory
  allocations and resources released, and `FrameStats::wait_ms` is
  begin_frame's wait for the GPU; the editor keeps both per frame and the
  gates print them for the two frames before each hitch (begin_frame
  waits for the frame two back).
- **GPU timing (N2 part 1).** `CommandList::begin_timing(name)` /
  `end_timing()` (nestable, up to 32 per frame) put timestamps around GPU
  work; `Device::gpu_timings()` and `gpu_frame_ms()` give the newest
  finished frame's times, read once its fence is reached, never waited for.
  The renderer times streaming, culling, raster, HZB, shading and
  anti-aliasing; the editor adds its UI.
- **States.** Each resource's state is tracked; `barrier()` moves it to what
  the next use needs, and render passes move their own targets. Buffers fall
  back to `common` after each frame, as D3D12 decays them.
- **Conventions.** Reverse-Z depth (1 near, 0 at infinity, GREATER passes),
  counter-clockwise front faces, back faces culled. Colour targets are sRGB
  (`rgba8_srgb`); UI draws through a UNORM view so its gamma-space colours
  show as picked.
- **Backends.** D3D12 on the DirectX 12 Agility SDK (shipped in a `D3D12`
  folder next to the exe), and a null backend that validates every call the
  way the debug layer would (plus stale handles and pass/pipeline mismatches)
  and emulates memory, clears and copies, so tests read results back without
  a GPU. Windows CI runs every GPU test on WARP with the validation layer and
  fails on any validation error.
- **Shaders** are Slang, compiled at build time by `giant_shaders()`
  (cmake/GiantShaders.cmake) to DXIL and embedded in the module; one Linux
  CI job compiles them too, to catch errors there.

The renderer (`render`) sits on top: plain per-frame data in (camera,
lighting, instances), an image out. Since N1 part 3 it draws clusters
chosen on the GPU into a visibility buffer, then shades it (see Geometry).

## Geometry

`geometry` holds meshes as plain data and everything done to them on the
CPU: glTF/GLB import (cgltf), and cluster LOD (meshoptimizer and its
`clusterlod.h`). It knows nothing about the GPU, so tools, tests and later
an offline cooker can use it without a device.

- **Clusters.** A mesh becomes clusters of up to 128 triangles and 128
  vertices. Groups of about 16 clusters are simplified to half their
  triangles with their outer border locked, then cut again; level by level
  until one group is left. Each group stores the error (metres) and bounds
  of its simplified replacement; errors only grow towards coarse levels.
- **The cut.** A cluster is drawn when its group's replacement would show
  more than `lod_error_pixels` of error and its own detail would not (or it
  is original). A spatial tree per level skips whole regions that are
  precise enough. Locked borders mean neighbours from different levels meet
  without cracks, so detail changes cluster by cluster, with no popping of
  whole objects.
- **GPU culling (N1 part 3, `shaders/cull.slang`).** The cut runs on the
  GPU, per instance: instances are tested against the view and the depth of
  the last frame, then the LOD trees are walked one level per dispatch
  through queues of (instance, node) items, with dispatch sizes written on
  the GPU (indirect arguments). Chosen clusters are tested against the view
  and the depth too, and appended to the visible list.
- **Two-pass occlusion.** Pass 0 tests against a depth pyramid (HZB, the
  farthest depth per texel, `shaders/hzb.slang`) built from the last frame,
  reprojected with the camera's movement. What it hides is kept aside and
  tested again in pass 1 against the HZB of what pass 0 just drew, so
  nothing visible is lost when the camera or objects move. The HZB of the
  finished frame is kept for the next.
- **Visibility buffer.** One raster pipeline draws every visible cluster:
  mesh shaders (one group per cluster, 128 threads) where the GPU has them,
  else a vertex shader that pulls 384 vertices per cluster instance. The
  pixel shader writes only (visible-list index + 1, triangle) into an
  rg32_uint target. A full-screen pass (`shaders/resolve.slang`) then
  rebuilds each pixel's triangle, works out barycentrics against the camera
  ray, and shades: one material evaluation per pixel, however dense the
  geometry.
- **Counters.** The GPU counts clusters, triangles and occluded work; the
  counts are copied to a readback ring and reach `RenderStats` a frame or
  two late (`from_gpu`). Full lists set an overflow flag (logged once).
  Slots and layouts shared by C++ and shaders live in `src/gpu_layout.h`.
- **Streaming (N1 part 4, `src/streaming.h`).** Cluster metadata (clusters,
  groups, LOD tree) stays on the GPU for every mesh, but the geometry
  (vertices, triangles) lives group by group in one fixed GPU pool
  (`RendererDesc::geometry_pool_bytes`, 256 MiB by default). A per-mesh
  residency table says where each group is, or that it is not loaded.
  Culling treats a missing group as precise enough, so the coarser clusters
  made from it draw instead, and asks for it; it also marks the loaded groups
  it used. The CPU loads what was asked for within a per-frame budget,
  parents before children (so the cut stays whole), and makes room by
  evicting the least recently used groups that have no loaded children. The
  coarsest level of every mesh loads first (within the same budget, meshes
  in the order added, except that a mesh the pool cannot hold yet does not
  block the rest; detail waits while any of it waits) and is never evicted;
  a mesh draws once all of it is in. Geometry comes from a CPU
  copy for now; world streaming (cells, from disk) will feed the same path.
- **Kits and shared materials (N1 part 5).** A kit is a named group of
  pieces (meshes, shapes, decals) kept in the scene; kit entities place it
  any number of times, and changing the kit changes every placement. The
  editor expands placements into instances each frame and gives equal
  materials one entry in a per-frame material table, which instances
  reference by index. An instance is 64 bytes (its 3x4 matrix, mesh,
  material and largest scale; the normal matrix is worked out in the
  shader). Draw calls do not grow with instances, meshes or materials:
  everything goes through the same raster and shading passes.
- **Decals (N1 part 5, `src/decals.h`, `shaders/decals.slang`).** A decal
  is a box that stamps a material onto whatever lies inside it, from its
  local +Y. The CPU projects each box to the screen and lists it in every
  16-pixel tile it covers; the shading pass tests only its tile's decals,
  in order, and blends their colour, roughness, metallic and emissive over
  the surface before lighting. Masks are shapes for now (disc, rectangle,
  ring, seeded splat and cracks), anti-aliased to the pixel's footprint;
  image stamps arrive with textures. Surfaces facing away from the stamp
  take none of it, and the box's top and bottom fade out.
- **Anti-aliasing (N1 part 6, `src/smaa.h`, `shaders/smaa.slang`).** No
  temporal anti-aliasing: nothing uses earlier frames, so the image is as
  sharp moving as standing still. Edges: SMAA 1x (High preset, no diagonal
  search) runs after shading in three full-screen passes (edges from luma,
  blend weights from SMAA's area and search tables, then the blend into the
  target); `RenderStats::post_draws` counts them. It is skipped in debug
  views. Shading's image is finished (screen reflections, tone mapping)
  into its own sRGB image first. Specular: in the
  shading pass, the change in normal across each 2x2 pixel quad (same
  instance only) widens the GGX roughness (Kaplanyan-Hoffman style, capped),
  so highlights on tight curves stay stable instead of flickering.
- **CPU path.** `gpu_culling = false` chooses the cut on the CPU (frustum
  only) and feeds the same raster and shading passes; tests use it for
  exact counts.

## World cells (N3 part 1)

The world is cut into square cells on the ground, 64 m a side
(`world::k_cell_size`, `world::cell_of`); an entity belongs to the cell its
position is in. Work goes by cell, so a change to one entity costs its
cell's work, not the world's:

- **The scene says what changed.** `world::Scene` hands out entities to
  change only through `modify` (reading is `find`, const), and logs the ids
  added, removed and modified; `take_changes` hands the log over. A new,
  copied or assigned scene, `clear`, or a log grown past the scene's size
  says "all". Kits and lighting are not logged (compared instead).
- **The editor keeps the frame by cell** (`editor/src/scene_render.h`).
  Each cell keeps its instances and decals; a frame rebuilds only the
  cells whose entities changed, or that place a kit that changed, and
  copies them into place. A new scene, an imported mesh, or a material
  losing its last user (the table is made again, so it holds only what is
  drawn) rebuild everything. Each cell keeps its place in the instance
  list from frame to frame (N3 part 3b): a cell that goes leaves a hole
  (instances with no mesh, which nothing draws, sent as a cell of its
  own, in pieces of at most 1024, each keeping its place and version
  until a cell takes it, so the renderer looks at it once; joined with
  the holes beside it to find
  room), a new cell takes the first hole it fits or the end (an empty
  cell sits at 0), and a cell that outgrows its place moves, with a
  quarter more room. So cells coming and going (streaming) leave the
  others where they are. When holes no cell has pass half the list (and
  4096), or push the list past `render::k_max_instances` when laying it
  out again would not, and on a new scene, the list is laid out afresh, end to end, by
  z then x (within a cell by id). `FrameInput::cells` covers the list
  end to end: each cell's (and hole's) place, size and version (new on
  every rebuild).
- **The renderer looks only at cells that changed.** The voxel world and
  sun shadows each keep a copy of the instances to find what moved; they
  compare only the cells not exactly as when they last looked (place,
  size and version, `src/cells.h`), so cells joining, leaving or moving
  cost only themselves. Otherwise (no cells, or fewer instances than
  before: the list laid out afresh) they compare everything, as before.
  A cell whose ground alone changed (its terrain or far ground tile mesh,
  rebuilt in place by the editor) says so: `CellRange::prev_version` and
  `changed_first`/`changed_count` name the part that changed, and one
  seen at `prev_version` is compared over that part only, not its
  thousands of scatter instances. Meshes coming and going (tiles stream
  all the time) make the voxel world and shadows look again only at the
  instances of meshes that went or changed bounds, found from a packed
  list of each copy's mesh.
  `RenderStats::cells` and `cells_changed` count them (holes included).
- **Instances stay on the GPU (N3 part 2, `src/instance_store.h`).** One
  buffer keeps every instance, in frame order, from frame to frame; a frame
  uploads only the cells not as before (all of them for a fresh layout,
  a new material table size, or without cells). Translations are kept from each
  cell's anchor (its first instance when uploaded; each cell keeps its
  anchor slot while it stays as it is, and new ones take free slots),
  which keeps them precise in float however big the world; each frame writes the anchors
  relative to the camera (16 bytes a cell) and a bit per instance saying
  whether it is drawn (its mesh ready, not left to the far field), and
  `load_instance` adds the instance's anchor back. Culling walks every
  instance and skips the unset ones; instances are addressed by their
  place in the frame everywhere. Per frame that is a bit per instance
  instead of 64 bytes.
- **Worlds stream by cell (N3 part 3, `editor/src/world_streaming.h`).** A
  world is a folder: `world.json` (`world::WorldIndex`: lighting, kits,
  the meshes its cells use, which cells have a file, the next id) and
  `cells/<x>_<z>.cell.json`, one per cell with entities or a terrain tile
  (`world::save_cell`: sorted by id, so an unchanged cell saves the same
  text; terrain in streamed worlds is under Terrain, below). Cells with any
  part within `load_radius` of the camera are read and parsed on a worker
  thread, nearest first, a few at a time; the main thread adds whole
  parsed cells to the scene up to `entities_per_frame` a frame and never
  waits on disk. Every `unload_every` frames, loaded cells past
  `load_radius + unload_margin` are let go, but only when what the scene
  holds of the cell is, entity for entity, what its file held (kept in
  memory while loaded): a cell with unsaved edits stays loaded until
  `save_world`. An
  entity moved (or put back by an undo) into a cell still on disk makes
  that cell load at once, and an entity id already in the scene is
  skipped when its cell loads (the scene's copy is newer). Saving writes
  only cells whose entities changed, through a temporary file and a
  rename, after `world.json` (with a next id past every id about to be
  written, and every cell about to have a file: a listed file that is
  missing loads as an empty cell); a cell on disk that the scene has
  entities in is merged with its file. An id met in a cell's file that
  the scene holds elsewhere, and was not read from that cell, is a clash
  (two files share it, or a new entity took it): the file's entity gets a
  new id, so neither is lost. Streaming runs only while the scene is the
  world's (`new_scene`, `load_scene` or undoing `open_world` pause it;
  undoing them resumes it). What streaming knows of the world's scene is
  kept with each copy of it an undo step holds (`WorldStreaming::memo`),
  taken when the undo or redo leaves that scene (unless a script replaced
  it since: a script's scene is never kept), so the copy put back is the
  scene as it was left, cells loaded since included. Which cells have a
  file stays as it is now; a cell whose file was written since the memo
  was taken is read again and merged, and the ids saved since are never
  handed out again. An entity's cell of origin is kept when its cell
  unloads, so an entity a redo puts back is never copied from the file it
  was saved to since (this costs about 40 bytes per entity met since the
  world opened: a later part bounds it), and a save writes again, without
  it, the file on disk of a cell an entity has left. Ids any of the
  world's scenes has handed out are never handed out again by another
  undo side. A save keeps the bookkeeping true to each file as it writes
  it, so one cut short by an error loses nothing either; a
  scene replaced any other way stops streaming. Opening another world is
  refused while the open one has unsaved edits, or its scene is replaced
  (it may be in the undo history), unless they are given up. The main thread does no work
  per frame that grows with the world: the cells to read are worked out
  again only when the camera enters another cell or a cell goes back on
  disk, and counts and the loaded cells are kept as they change. Loaded
  cells also keep their file's entities in memory (twice the entity
  memory of what is loaded). A material falling out of use no longer
  rebuilds every cell: unused entries stay in the table until they
  outnumber the used ones (and 256).
- **Known limits, next parts.** The CPU still looks at every instance each
  frame to decide which are drawn (mesh ready, far field) and which cast
  shadows: cheap per instance, but whole-cell tests come next. Streamed
  out cells are gone from the voxel far field too (cells need saved voxel
  stand-ins for that, later). While many cells stream in at once (more
  than an eighth of the scene in a frame, or over 4096 64 m squares of
  changes waiting) the voxel world still rebuilds every window; holes cost a few bytes per
  instance a frame in the loops that walk every instance. Meshes are imported when a world opens (the
  index lists them), not as cells stream. Unloading a cell is one
  `Scene::remove_many`, a pass over the scene's entities, and the unload
  look walks the loaded cells. Undoing an edit to a cell that was saved
  and unloaded since does nothing. A world cannot yet be saved to another folder, and its kits cannot
  be deleted while some cells are not loaded. Decals overlapping across a cell border draw in cell
  order, not the outliner's.

## Terrain (N3 part 4)

The ground is a height field (`world::Terrain`, `engine/world/include/giant/world/terrain.h`),
cut into tiles, one per world cell, kept in `Scene::terrain`:

- **Samples.** A tile holds `samples()` x `samples()` heights (8 to 128 a
  side, a power of two: 8 m to 0.5 m apart; 64, 1 m, by default; set only
  while there are no tiles), the first at its cell's corner. The surface's
  last row and column come from the neighbours; `Terrain::sample(i, j)`
  takes a height by whole-world sample number, from its tile or, when that
  tile is missing, from the sample before it on x, z or both. Tiles
  sharing an edge always see the same heights and normals along it; a
  tile touching another only at a corner, with neither tile between them,
  keeps its own corner (no spike, and no edge to crack along). `height_at(x, z)` is the surface exactly as the finest
  mesh draws it (two triangles a square, split on the (0, 0)-(1, 1)
  diagonal).
- **Changes.** The terrain logs the tiles set, changed (`modify`) and
  removed, as the scene logs entities (`take_changes`; a new, copied or
  assigned terrain is "all").
- **Meshes** (`editor/src/terrain_meshes.h`). Each frame `TerrainMeshes`
  takes the changes and queues the changed tiles and their eight
  neighbours (a tile's mesh reaches one sample into them). It looks at a
  few queued tiles a frame (8 x `builds_per_frame`): a snapshot of the
  heights the mesh is made from (the tile's rows copied, the ring around
  them looked up), hashed; a tile whose snapshot matches what it draws
  (a neighbour whose shared edge did not change, or a whole terrain
  copied by an undo) is not built. The rest are built on two worker
  threads (`builds_per_frame` started, `builds_in_flight` at once): the
  job makes the triangles and normals and runs `geometry::build_clusters`
  with the tile's edge vertices locked (`meshopt_SimplifyVertex_Lock`),
  so every level of detail keeps the edge and tiles drawn at different
  detail meet without cracks. Finished meshes are uploaded a few a frame
  (`uploads_per_frame`) and replace the old one only once
  `Renderer::mesh_ready` says they can draw, so a tile never blinks out.
  A tile changed while it builds is built again with the newest heights.
  The renderer hands a removed mesh's id (and its streaming group
  numbers) to the next mesh added, so rebuilt tiles do not grow the mesh
  table every frame walks; callers stop drawing an id when they remove
  it.
- **Drawing.** `scene_frame` puts each built tile first in its cell, at
  the cell's corner, with the terrain's material and
  `render::Instance::always_mesh`: the far field never stands in for it
  (the CPU never leaves it to the voxels, its voxels are not stand-ins,
  and the resolve pass does not skip stand-in voxels around it, so far
  trees on a tile still show). It is in the voxel world like any mesh, so
  it casts shadows and takes part in GI and reflections. `always_mesh`
  travels to the GPU in the top bit of `gpu::Instance::material`.
- **Saved as text** with the scene (`"terrain"`: `samples`, `material`,
  `tiles` of `cell` and `heights`, one string of numbers per row in the
  shortest form that reads back exactly).
- **In streamed worlds (N3 part 4b).** A cell's file holds its tile too
  (`"heights"`, rows of text as in a scene), so a tile loads and unloads
  with its cell: a cell is let go only when its entities and its tile are
  exactly what its file holds. That check is one compare: every tile has a
  version (`Terrain::version`, new on each set or modify, unique across
  terrains, kept by copies and put back by undo), and a loaded cell keeps
  the version its file's tile had in the scene, so no second copy of the
  heights is kept and nothing compares them. A tile weighs samples^2 / 64
  entities against `entities_per_frame` (64 at 64 samples), and at most
  `unloads_per_frame` cells (128) go in one frame, the rest in the frames
  after; their meshes are freed `drops_per_frame` (64) a frame. A tile new
  to `TerrainMeshes` waits `settle_frames` (6) with its neighbours before
  it is built, so a burst of tiles (a world opening) builds each once
  (changes to tiles already there do not wait). Flying, a tile at the
  edge of the loaded area is still built again when the next row arrives
  and when a neighbour unloads (its edge reaches into them): about 2.8
  builds per tile. `world.json` holds the terrain's `samples` and
  `material` and which cells' files hold a tile (`terrain.cells`). A tile made or put back by an undo in a cell still on
  disk makes the cell load at once, and the scene's tile wins over the
  file's; saving such a cell keeps the file's entities (and its tile, if
  the scene has none there). All tiles are one spacing: while any file
  holds a tile, `create_terrain` takes the world's spacing and refuses
  another, streaming gives an empty terrain the world's spacing back (an
  undo can take it away), a file whose tile is another spacing fails to
  load alone, and a save that would mix spacings is refused.
- **Far ground (N3 part 4c).** Past the loaded cells the ground is drawn
  from coarse copies of the terrain (`engine/world/include/giant/world/far_ground.h`,
  `editor/src/far_ground.h`), a quadtree of branching 8: level 1 chunks
  are 512 m (8 x 8 cells) with 32 x 32 heights 16 m apart, level 2 chunks
  4096 m (8 x 8 level-1 chunks) with heights 128 m apart. A chunk's heights
  are the finer level's at its own sample points (no averaging, so levels
  agree where they share a point); NaN where there is no ground (a cell
  with a tile has all 16 of its samples, one without none). Each level is
  a `world::Terrain` of 512 m or 4096 m tiles that may hold NaN, with its
  own `TerrainMeshes`, so chunks mesh like tiles (triangles with a corner
  where there is no ground are left out). Files: `ground/1/<x>_<z>.ground.json`
  and `ground/2/...` (format `giant-ground`, 32 rows of numbers, `-` for
  none), written by `save_world` after the cell files over every cell whose
  tile changed: the chunk's old heights where nothing changed, the tiles
  (the scene's, or read from their cells' files when the chunk had no file
  yet) elsewhere; a chunk left with no ground loses its file, and the save
  hands what it wrote to the far ground at once. The editor reads chunks
  on a worker within `far_radius_1` (8192 m) and `far_radius_2` (32768 m)
  of the camera, adds `chunks_per_frame` (16) a level a frame and lets go
  of ones past the radius plus a margin (512 m, 4096 m). What draws:
  a level-2 chunk draws its mesh unless every level-1 chunk in it can
  stand in (its mesh is drawn, every cell of it with ground has its full
  tile drawn, or it has no file); a level-1 chunk under no drawing parent
  draws its cells' full tiles when all of them are drawn, else its mesh,
  else what tiles it has; a full tile draws only where its level-1 chunk
  says so. The choice is worked out only for chunks whose inputs changed
  that frame (a tile's or chunk's mesh came, went or was replaced; a
  chunk was added or let go). Chunks draw from the cell at their corner
  (always_mesh, the terrain's material). Every tile and chunk hangs a wall
  from its edges (`TerrainMeshes::skirt_depth`: 8 m full tiles, 64 m level
  1, 256 m level 2, seen from both sides) that fills the gaps where tiles
  of different spacing meet. Cells with ground load and unload by their
  level-1 chunk's distance, so a chunk near the camera has all of its
  tiles; a chunk with an unsaved tile keeps all of its tiles loaded, so
  near edits always draw in full (the chunk files are the saved ground).
- **Known limits, next parts.** The far ground shows edits only once
  saved (a chunk far enough to draw coarse shows its file). Level 2 takes
  only every other cell's corner, so ground narrower than 128 m (a lone
  tile) may not show at all at that distance, and a level-2 chunk whose
  points make no square draws nothing (its children do as they can).
  `far_radius_1` is taken as at least `load_radius` + 5800 m (a level-2
  chunk's diagonal), so a level-2 chunk never draws over the full tiles
  by the camera. A chunk file that cannot be read stands in for nothing
  (its full tiles draw where loaded), and a cell whose file failed is not
  waited for. The coarse ground stops one of its own samples short (16 m,
  128 m) at the edge of the ground, where the tiles' walls hang. A save
  cut short owes its chunks to the next save, kept in `world.json`
  (`terrain.far_owed`, `far_owed_2`: written before the cells, cleared by
  the save that writes them), so a crash or reopen does not lose them. Every tile gets the 8 m edge walls, streamed world or not.
  A world saved before part 4c has no chunks until its tiles
  are saved again. Every loaded
  tile is a full mesh in memory (CPU copy and cluster data: about 0.4 MB
  a 1 m tile, 1.2 MB at 128 samples; the default radius loads about 1,800
  cells, more now that ground loads by whole 512 m chunks), and a command may make at most 16.7M heights, the terrain hold
  67M (16,384 tiles of 64 samples, 4,096 of 128). Locked tile edges keep
  every level's edge, so a tile never drops below about 4 x samples
  triangles (256 at 64 samples) however far. An undo step holds copies of the tiles a command changed
  (before and after), and a scene swap's undo side the whole terrain. One
  material for all of it (changing it rebuilds every cell with ground,
  once); painted layers and erosion come later. Like lighting, a changed
  terrain material does not count as an unsaved cell.
- **Terrain editor (N3 part 5).** Brushes (`engine/world/include/giant/world/terrain_brush.h`):
  a round brush of a radius, full strength inside `radius * (1 - falloff)`
  and easing (smoothstep) to nothing at the rim, dabbed at least every
  quarter radius along a stroke. Raise and lower add metres; flatten moves
  towards a height and smooth towards each sample's 3 x 3 average (read
  from a copy taken before the dab, across tile edges), both by up to the
  strength (0 to 1). Each sample belongs to one tile, so a dab over a tile
  edge changes both tiles' own samples and their meshes stay joined. Only
  tiles that exist change, and a tile no height of changes is left as it
  is (no new version, no undo step). Smooth copies only the tiles there
  are. `sculpt_terrain` refuses a call over 4096 cells, 65536 dabs or 16.7
  million heights reached (dabs times each one's square), so one call is a
  bounded hitch; ground saved in an open world but not loaded is left as it
  is (`cells_not_loaded`; the loaded tile's edge can then meet it in a
  slope, as smooth reads no heights there). One undo step of a drag (its calls merged:
  `CommandContext::merging`) holds each tile once: as it was before the
  drag first changed it, and, taken when undone, as it was after (for
  redo). The viewport brush (`editor/src/terrain_brush_tool.h`) casts a ray
  from the camera through the pointer, marches it over the ground (half
  the sample spacing near, steps growing a 500th of the distance) and
  refines the hit, and sends one `sculpt_terrain` a frame while the left
  button is held (only when it moved a quarter radius, or 30 times a second
  held still, so the frame rate does not set the speed; no ray is cast
  until the pointer or camera moves or a dab is due), in one merge group:
  the stroke is one undo step. A move of over 4 radii is bisected along
  the pointer's path (up to 6 more rays), keeping the half least like
  smooth ground: its gap against the pixels between times the ground a
  pixel covers at its ends (distance x pixel angle / cosine to the ground,
  floored at 1e-5;
  their geometric mean is exact for flat ground at a grazing angle). A
  fast drag over far ground stays one line; a gap of over 4 radii and 4
  times what smooth ground would give (the pointer over a ridge), or a
  jump since the camera moved, starts the line again where it lands. The panel
  keeps the radius to 128 samples (`BrushSettings::max_radius`), and
  flatten goes to the ground where the stroke began unless a height is
  given. Far ground (past the loaded tiles) cannot be sculpted. Height maps
  (`engine/world/include/giant/world/height_map.h`): 16-bit RAW (.r16,
  .raw; square, little-endian) and PGM (P5 or P2, 8 or 16 bit), up to
  16384 a side, sampled bilinearly into tiles at the terrain's spacing; a
  `to` on a cell's edge also sets the first samples of a tile past it.
- **Scatter (N3 part 6).** Rules for strewing things over the ground
  (`engine/world/include/giant/world/scatter.h`; the layer types are scene
  data in `scene.h`, saved with scenes and in world.json). A layer's points
  come from a global grid of `spacing` squares, one point a square jittered
  inside it (no two closer than spacing x (1 - jitter)), each square's
  numbers from a hash of the seed and the square, so a point is the same
  whichever cell asks and cells share none. Points are kept by density, a
  value-noise clump mask, the area, and the ground's height and slope
  (normal from heights a sample either side); each gets an item by weight,
  a turn about up, a scale, and a tilt towards the normal (`align`).
  `scatter_cell` reads only the cell's tile and its neighbours', so the
  editor's `ScatterPlacer` (`editor/src/scatter_placer.h`) copies those 9
  tiles to a worker job (2 workers) and swaps a cell's points in whole
  (within `instances_per_frame`, one cell at least). Only cells within
  `radius` (2048 m) of the camera are placed, nearest first; a cell draws
  at most 16384 instances (a frame swaps in one whole, about 1.5 ms;
  otherwise `instances_per_frame` is 4096, since every part of the
  renderer looks at each placement the frame it arrives) and
  all cells together `max_instances` (524288; a kit item counts its
  pieces, and a kit's piece count is part of every cell's key). A cell
  arriving over the budget takes room from farther cells, cells out of
  range go before any arrive, and cells cut short try again when room
  frees. The scene's instance list reserves room for scatter once, so it
  never doubles in a frame. A cell is placed again when its
  tile or a neighbour's changes (TerrainMeshes::last_changes) or the layers
  change (compared each frame), and keyed by the layers and those 9 tile
  versions, so an unchanged cell is never placed twice. Placements are not
  saved and not entities: `scene_frame` draws them after the cell's
  entities (items' meshes and materials looked up once a cell, kits
  expanded), so they stream with the ground, reach the voxel world and far
  field like any instance, and a kit change rebuilds scattered cells. At
  most 2^18 points a cell (`cells_cut_short`). Not yet: masks from
  entities (no trees in houses), layers avoiding each other, painted
  density, and destruction removing placements.
- **Weather and sky (N3 part 7).** `world::Weather` (`weather.h`) is
  optional scene data (saved in scenes and world.json); without it the flat
  sky colours stay exactly as before. The editor's `WeatherState`
  (`editor/src/weather_state.h`) is view state: it follows the scene's
  weather, blends towards a new one over `over_seconds`, and carries the
  cloud pattern along on the wind (an offset, so the clouds move without
  history). `weather_light` turns sun height, haze and cloud cover into
  the sun, sky and ground light the shading uses (the sun through the air
  by air mass, the sky greyed by cloud). The renderer gets a `Sky` in
  `FrameInput` and runs, before shading (`sky.slang`, `sky_common.slang`):
  a 192x108 sky-view table of single scattering (Rayleigh and Mie, round
  earth), a 512^2 tiling cloud pattern made once, a 256^2 cloud shadow map
  over 32 km about where the sun's rays from the ground near the camera meet the cloud base (snapped to texels; past it, the clouds' mean shade), and half-resolution
  volumetric clouds (a shell between base and top, a fixed dither, no
  temporal history: rule 2) written as light and transmittance plus the
  distances to the first cloud and to the scene. The resolve upsamples the
  clouds by nearest depth, composites them over sky and scene, adds height
  fog (exponential in height, capped at 30 km) and darkens the sun by the
  cloud shadow (screen reflections are added scaled by what of the pixel comes through). A change of cloud size rescales the wind offset about the camera, so the clouds overhead stay put. Settings `clouds`, `cloud_steps` (48) and `cloud_shadows`.
  Not yet: lightning, clouds parting round giants.
- **Rain, snow and wet ground (N3 part 8).** `world::falling` splits the
  weather's precipitation into rain and snow by temperature;
  `world::weather_ground` steps the ground's wetness and snow cover
  (exponential approaches in steps of at most 10 s, so any step size
  agrees) from the scene's start values when weather first shows or the
  scene is replaced; the editor's `WeatherState` carries it on each frame
  under the shown weather (view state, like the blend), and set_weather's
  `wetness`/`snow_cover` put it somewhere at once. The renderer's `Sky` takes what falls, the wind,
  the ground and a time. In the sky passes a cover map (256^2, a metre a
  texel round the camera, r32) marches the voxel world straight down per
  column (from 800 m above, or the coarsest window's top, to the finest
  window holding it, then down that window) and stores the height of what
  is on top, less the size of the voxel it hit (the march reports it,
  whichever level or stand-in brick it was found on).
  `weather_surface` (sky_common.slang) then wets surfaces open to the sky
  (darker by porosity, rougher surfaces most; glossier; puddles from a
  noise that repeats every 4096 m in the hollows of flat ground) and lays
  snow on what faces up, before shading, on meshes and far-field voxels
  alike (walls judged from the air beside them). Voxel-cone reflections
  follow the dry roughness, so rain does not put a cone on every open
  pixel (the wet gloss shows in screen and sky reflections). `apply_falling`
  draws rain streaks or snowflakes on five cylinders round the camera (1.5
  to 24 m), a hash per cell of a fixed size per cylinder, falling whole
  cells per 64 s cycle so the time wraps without a jump, the drops and
  their streaks leaning with the wind (a bounded shear, so cells never fold
  across columns; not carried round, which would differ by view), none
  where sheltered: no particles, no history. Settings `precipitation`, `wet_surfaces`,
  `weather_cover`. Not yet: ripples, splashes, drips, wet voxel colours for
  GI, snow depth (it is a coat, not a volume).

## Physics (N4 part 1)

`giant::sim` holds the physics world (`sim/physics.h`), Jolt underneath
and kept private: callers see plain types (`BodyDesc` of box, sphere,
capsule, convex hull and height-field parts; `BodyId`; `RayHit`). Jolt is
built in double precision, so bodies sit at world positions tens of
kilometres out without a moving origin. Steps are fixed at 1/60 s, at most
four a call (the rest is dropped and counted, so a slow frame never
snowballs into more work), and run on a `JobSystem` of their own: the
editor gives physics half the cores, two to four, so it never waits on
the voxel, scatter or terrain workers.

`ScenePhysics` (`sim/scene_physics.h`) keeps the physics world in step
with a `world::Scene`: each entity whose shape collides gets a body made
from what the renderer draws (a cube a box, a sphere a ball or, scaled
unevenly, a hull, a plane a 0.5 m slab whose top is the plane, a kit one
fixed compound of its pieces; sizes under a millimetre count as one), and
each terrain tile a height field of its own samples plus its +x and +z
neighbours' edge (the same triangles the mesh draws). It reads the change
list the frame took (`SceneFrame::take_for_followers`) and the terrain's
own second change log (`Terrain::take_physics_changes`), so a frame looks
at what changed, not the scene's size; a moved entity is teleported, a
resized one remade, and a body that goes or moves wakes what slept on it.
`Entity::body` says how an entity collides (`fixed`, `dynamic`, `none`)
and `Entity::mass` its weight (0: from its size, water's density); both
are saved only when not the default. Meshes and decals do not collide
yet, and kits stay fixed whatever their body (they break rather than
fall, N4 part 4).

Simulated motion never goes into the scene. The dynamic bodies' places
are poses beside it (`ScenePhysics::poses`), and the editor draws those
entities there (`scene_frame`'s `poses`, rebuilding the cells of the ones
that moved); the scene keeps where things were put, which is what undo,
saving and streaming see. Simulating in the editor
(`editor/src/physics_state.h`) is view state, like a play button: stopping
drops the poses and puts the bodies back where the scene has them. The
bodies follow the scene while stopped too, so starting costs nothing and a
raycast answers at once. A body remade while simulating (its size, body
or mass edited) starts again from where the scene has the entity.

## The player (N4 part 2)

`PhysicsWorld` also holds characters (`add_character`, `CharacterDesc`):
Jolt's `CharacterVirtual`, a capsule that is moved rather than pushed
about. Each fixed step, before the bodies, a character's velocity comes
from its input (a wanted speed over the ground, reached within a set
acceleration on the ground, slowly in the air, plus a jump asked for in
the last fifth of a second); then it is swept through the world, sliding
along walls, walking up steps, sticking to the floor going down them and
sliding off slopes over its limit. Jolt keeps the speed it was asked for,
not the one it got, so the character keeps both: how it really moved (a
wall stops it) and its own speed up or down (so a step up is never taken
for a leap). A kinematic body inside it lets rays and falling things meet
it. `PhysicsWorld::clear` keeps characters, made again where they were.

Play mode in the editor (`editor/src/play_state.h`) puts one on what is
under the camera, turns simulating on and puts the camera at its eyes,
drawn between the last two fixed steps (`PhysicsWorld::step_fraction`) so
it moves smoothly at 144 Hz as at 60, and eased up steps; the window's
relative mouse mode gives the look, and the panels get no mouse and only
key releases meanwhile. Starting never flushes the bodies (a stall in a
world just opened): it queues a frame's share and holds the player at the
camera until no entity waits and the ground under it is in
(`ScenePhysics::entities_waiting`, `ground_waiting`; five seconds at most),
then stands it on what is under the camera. It also holds it
(`hold_character`) while the ground tile under it is still being made
(`ScenePhysics::ground_waiting`), so streaming in never drops it through.
Only the camera is drawn between steps: bodies are drawn where the last
step left them, so a platform it rides can lag the view by up to a step
(16 ms) until poses are blended too.
Like simulating it is view state: stopping removes the character and puts
the camera and simulating back as they were.

### Climbing and being carried (N4 part 9)

A character told it may climb (`CharacterInput::climb`, on for the player)
takes hold of what it walks into when a ray from its chest finds a surface
too steep to walk, facing it, within a third of a metre, and not a dynamic
body (a crate is pushed, not climbed). On the wall its step is its own,
before the walking step: its input turns into the wall's frame (into the
wall is up, away is down, across is along, at `climb_speed`), it is held
5 cm off the surface (re-found by a ray each step, so it follows round
corners of a convex wall and a moving wall's surface speed), and swept
with no gravity. It lets go when the ray finds nothing to hold (past an
edge, or what it held came loose and is dynamic now), when climbing down
reaches the ground, when told not to climb, and when a jump pushes it off
(away and up; no hold again for 0.4 s); a jump asked for as it meets a
wall is a plain jump, not a push off. At the top (the chest ray hits, one
over the head does not) a ray down beyond the edge looks for ground it can
stand on: if its shape (a little smaller) fits risen beside the wall,
halfway over and there, it mantles, up for 0.27 s and then over the edge
for 0.18 s, moved directly. On a ledge that moves (not static) the path is
kept in the ledge's frame, so it lands where the ledge is now. Climbing
speed, the wall's surface speed included, is held to the characters' cap.

Any body that is not a character's can carry one (`carry_character`): the
character's place in the body's frame is kept, and after each step it is
put there, its speed the body's speed at that point (so a body put
somewhere at once, as `set_entity` does, carries it there without
flinging it on when let go), and what it touches is found again there (so
held up in the air it is not on the ground). Its input is left aside. `release_character`,
`set_character_position`, the body removed or the world cleared let it
go, and it falls on from there. The editor's `carry_player` carries the
player by an entity's body; an entity whose body is made again (its
shape or kind changed) lets it go. It is the start of a giant's hand
picking the player up: the hand will be a kinematic body moved with the
animation.

## Breakable buildings (N4 part 4)

A kit marked `breakable` (with a `strength` in N·s, default 1500) is still
one fixed compound body while whole, but its contacts are reported piece by
piece: `PhysicsWorld` keeps a contact listener that, for each new contact,
records the two bodies, the parts struck and the impulse (closing speed at
the point over the two bodies' inverse effective masses there: inverse mass
plus the turning share, (r × n) · I⁻¹ (r × n)), keeping hits of at least `min_hit_impulse` (50
N·s). Manifold reduction is off for those bodies (`BodyDesc::report_parts`)
so the part struck is the one named.

`ScenePhysics` makes a support graph for a placement the first time it is
struck: pieces whose bounds lie within 2 cm of each other touch, and those
within 5 cm of the kit's lowest bottom stand on the ground. After each step
(`break_from_hits`) a piece hit harder than the kit's strength comes loose;
then a breadth-first walk from the ground pieces finds what still stands,
and every piece not reached comes loose too (a roof whose walls are gone).
The standing pieces are rebuilt as one fixed compound; each loose piece
becomes a dynamic body of its own, up to 2 cm smaller across than drawn (so neighbours
don't wedge it) with a small tipping spin (so a wall falls rather than
balancing). A building's own falling pieces never break the rest of it, so
it does not shake itself apart. `impact` is the other way in: a blow at a
point over a radius, for kinematic movers (a giant's feet) that make no
contacts with fixed bodies.

As with all simulating, it is view state: loose pieces are drawn from
`piece_poses`, the scene is untouched, stopping (`reset_poses`) mends every
building, and a real edit of a placement (moving, resizing, deleting, a
kit change) mends it too, while a colour change keeps the break.

Known limits: `impact` walks every entity (fine for a town; a cell index
later); loose pieces stay in their kit's render cell however far they roll;
mesh and decal pieces never come loose (only cubes, spheres and planes
collide, and no sphere flattened under a centimetre); the part index assumes every piece built a shape; damage is not
saved (N4 part 8); pieces break whole, never into smaller bits.

### Falling trees (N4 part 5)

A scatter item can be a tree (`ScatterItem::tree`: trunk radius and height
at scale 1, and a strength, N·s, default 8000, grown with the scale
squared). `sim::Forest` holds the trees of the scatter cells near the
camera: the editor's `PhysicsState::feed_forest` hands it each cell's tree
points whenever the placer swaps in a new placement
(`ScatterPlacer::Cell::serial`), at most 32768 points looked at a frame (or
one cell), nearest cells first. Within `radius` (384 m, plus a cell) every
tree stands as a fixed capsule from half a radius above its foot (past what
is sunk) to its top, at most 65536 of them (a nearer cell takes them from
the farthest); past a cell more the standing trunks go, and a cell without
fallen trees is forgotten. Trunks are made and removed 1024 a sync at most
(about a millisecond; rule 9): the bodies of trees let go (far, evicted, a
cell placed again or gone) wait in a queue for that budget.

After each step `Forest::after_step` reads the same contact hits as the
breakable kits: a standing trunk struck at least its strength is removed and
made again as a dynamic capsule (wood, 700 kg a cubic metre) turning about
its foot away from the blow, faster for a harder one (0.4 to 1.5 rad/s).
Falling, it is an ordinary body: a tree it strikes hard enough falls in
turn, and a breakable kit's pieces it strikes come loose (`ScenePhysics`
sees the hit too). A hit's impulse counts the struck point's effective
mass (`PhysicsWorld` hits: mass and inertia), so a falling trunk's tip
strikes far softer than its whole mass, and the default strength keeps a
tree of the same size from felling its neighbour: no chain through a whole
forest. `impact` reaches trees as well (the distance is to the trunk's
segment, foot to top). At most 64 trees start falling a step, the hardest
struck first, 512 move at once and 16384 lie fallen; blows past that are
shrugged off (counted). A fallen tree at rest sleeps (costing the steps
nothing) and wakes, falling on, when what it rests on goes (a tree it leant
on felled, a building's piece come loose).

Fallen trees are view state like loose pieces: `Forest::fallen()` gives,
per cell, the placement serial and the fallen points' foot transforms (the
body's pose with the point's scale). `emit_scatter` draws a cell's fallen
trees after all its other points, so while the same trees are falling only
their instances are made again, in place (`rebuild_fallen`, telling the
renderer only that range changed, as a ground-only change does); a tree
starting to fall or standing up again rebuilds the cell once, at most 8
cells a frame (`SceneFrame::fallen_rebuilds_per_frame`; the rest wait for
the next frames, drawn as they were). A stale list
(another serial) is never drawn. Stopping simulating stands every tree up
again; a cell placed again does so for its trees; a new scene forgets the
forest's ids as the scene's bodies clear the world whole
(`ScenePhysics::clear(others)`).

Known limits: a tree falls whole from its foot (no stump, no splintering;
the destruction rings are N4 part 6), and the foot can slide a little as it
tips since nothing pins it; fallen trees are not saved (N4 part 8) and
stand again when their cell is placed again (a ground edit, the camera
leaving the placer's radius); trunks stand only near the camera, so a tree
farther than about 450 m never falls; the player's and a giant's kinematic
bodies make no contacts with fixed trunks (as with buildings: `impact` is
the way in).

### Destruction near and far (N4 part 6)

What breaks costs by distance from the camera, in rings:

- Near (within `ScenePhysicsSettings::simulate_radius`, 600 m, and at most
  `max_loose`, 4096, loose pieces as bodies at once): a loose piece falls
  as its own body (part 4), a felled tree as a dynamic trunk (part 5).
- Mid (past that, or past the cap): a loose piece comes down at once as
  rubble with no body (`ScenePhysics::rubble_at`): thrown along its push as
  far as half a second carries it (10 m at most), laid with its thinnest
  side down, turned about the up axis by a hash of its placement and index,
  on the first fixed body a ray down finds that is not its own building
  (what is falling, or may yet fall, would leave it in the air); at most
  `rubble_rays` (4096) rays a blow, past them, or when nothing is found,
  at its building's foot. A piece the physics world refuses a body comes
  down as rubble too. A tree whose
  trunk has no body (beyond about 450 m, or still waiting for it) struck
  by `impact` is laid down at once (`Forest::lay`): tipped about its foot
  away from the blow until its top meets the first fixed body a ray down
  finds there that is not a standing trunk (45 to 108 degrees over), at most `lays_per_impact`
  (2048) a blow, hardest first. The editor gives the forest the scatter
  cells the blow reaches first (`PhysicsState::impact`, nearest first, at
  most 65536 points), and keeps a far cell only while trees in it lie.
- Far: the voxel world follows the instances, so rubble and fallen trees
  show in the far field as they are drawn (nothing new).

Dust (`sim::Dust`): a collapse throws up a puff at the foot of what came
down (sized to it, 2 to 60 m), a felled tree one at its foot, a falling
tree landing hard (a quarter of its strength) one where it strikes, and a
laid tree one along its middle. A puff grows to three times its size over
its life (8 to 30 s, by size), rises 0.4 m/s, drifts with 60% of the wind
and thins (its dust across it kept, faded over the last 40% of its life).
Bounded: 512 puffs (a new one takes the faintest's place), 64 emits
between steps, and an emit inside a puff younger than a second swells it
instead (a street falling is one cloud). The renderer draws the 32 nearest
(`FrameInput::dust`, `render::DustPuff`): in the resolve pass each puff's
density (1 − r²/R²)² is integrated along the pixel's ray exactly, clipped
to what the pixel shows, lit by the sun (a mildly forward phase, doubled
for light scattered inside the cloud) and the sky and ground, over the
fog and clouds (over the flat background, without the sky, exposed and
tone mapped as it is not), thinning the screen reflection's weight alike.
A puff emitted into a young one swells it with its growth carried on, not
compounded. No history
(rule 2). Dust is view state: stopping or a new scene clears it.

Known limits: rubble and laid trees have no bodies (the player walks
through them, and they stay as laid when what is under them goes); rubble
lies flat to world up (half sunk on a slope); a far cell placed again (a
ground edit) stands its laid trees up, as near ones do; dust
ignores shadows (a puff in a building's shade is lit as in the sun) and is
drawn over the fog rather than in it; pieces still break whole (no
splintering); craters and footprints are part 7.

### Craters and footprints (N4 part 7)

`world::Dent` is a dent pressed into the terrain: a smooth bowl,
`-depth (1 - e²)²` for the elliptical distance e under 1, with a rim
`depth × rim × (1 - t²)²` out to 1.5 (`apply_dent`, `dent_offset`); a
footprint is the same stretched along a yaw. While simulating,
`PhysicsState::dent` (the `dent_ground` command) presses it into the
scene's terrain itself, so everything that follows the ground follows
with its own budgets: the terrain meshes, the physics tiles (a resting body
wakes as its tile is swapped), the voxel world, scatter (re-placed onto the
new ground). It is still view state: `world::GroundDamage` keeps each
dented tile's heights and version as they were before its first dent, and
stopping puts them back with `Terrain::restore` (the same versions, so
nothing reads as edited or unsaved and every cache stays good). Every
terrain command, every undo and redo of a scene change, a new scene's
copy for its undo, `save_world`, `save_scene` and `open_world`'s unsaved
check run on the ground as authored (`PhysicsState::GroundAsSaved`: the
dents lifted, then put back with their own versions, so nothing that
follows the ground sees a change); a tile such an edit changes loses its
dents, so a snapshot never holds one and nothing brings one back for good.
`simulate off` puts the ground back at once. A dented cell reads as
unsaved meanwhile, so it never unloads.

A cell placed again keeps its fallen trees that stand where they stood
(`Forest::set_cell`: the same x and z to the centimetre, size and model,
whatever their point numbers now: a dent adds or takes other layers'
points). A dent re-places the scatter of the cells it touches; the editor
gives the forest those cells as soon as the placer swaps them in
(`PhysicsState::refresh_forest`, the same frame, so felled trees are
never drawn standing), wherever they are, and the trees felled there stay
down (their bodies wake and settle into the dent; laid ones move with the
ground under their foot).

Cost: a dent walks the samples in its own turned bounds once (radius times
stretch is at most 200 m: about 1.4 million samples at the finest
spacing, a one-off), and keeps a copy of only the tiles it changes.

Known limits: rubble (no body) stays at the height it was laid; stopping
puts every dented tile back in one frame; damage is kept only by
keep_damage (part 8).

### Kept damage (N4 part 8)

Simulating is view state; `keep_damage` makes what it broke part of the
world. `world::CellDamage` (damage.h) is a cell's kept damage, beside what
was authored there: `KeptKit` records (a breakable kit placement's id,
kit and transform as they were, and each piece down with its world
transform) by the cell of the placement, and `KeptTree` records (a scatter
tree's foot x and z and size, and its lying transform) by the cell of the
foot. Craters are not records: `GroundDamage::keep` leaves the dented
tiles as they are, now the ground as authored (the command records their
heights and versions before and after, so undo puts each back exactly).

`Scene::set_damage(cell, damage)` holds it (copied with the scene, saved in
scene files as `damage`, in cell files as their `damage` key) and marks the
placements its records name as changed, so `ScenePhysics` re-syncs them: a
placement whose record still matches it (`kept_of`: same id, kit and
transform, a breakable kit) is made with those pieces down where the
record says, with no bodies (`Broken::Piece::kept`), and the rest standing
as one body (`standing_body`, shared with `loosen`). That holds whether
simulating or not; `reset_poses` leaves a placement broken only as kept,
and remakes one broken further back to its kept state. A real edit (moved,
resized, another kit) no longer matches: it stands whole, and the next
`keep_damage` drops the record. `Forest::set_cell` takes the cell's tree
records: the tree standing at the same x and z (to the centimetre), as
big, lies where its record says, with no body, and stays down through
`reset`; one fallen there now gives its place to the record. The editor's
`PhysicsState::follow_kept` gives the forest a cell it knows again at
once whenever the scene's kept trees there differ from what it was given
(`damage_edits` says when to look); a cell it does not know yet (streamed
in, wherever it is) waits until the placer has placed it and is given
within the feed's budget. Kept trees never count against `max_fallen`, and
a record that finds no tree now (a placement cut short) is kept for when
it comes back.

An undo or redo sets a cell's damage with `hold_empty`: an empty entry
held in the scene says "none here", so for a cell streamed out since, the
scene's side wins over the file's when it comes back, as an entity put back
by an undo does. Two cells may name one placement (a stale record kept
before it moved): `kept_kit(id, &placed)` finds the one made where it is.

Streaming treats kept damage as it does the terrain tile: read with the
cell (`load_cell`'s damage) and set into the scene on insert (one the
scene already holds, put back by an undo, is newer), compared with what
the file holds for unsaved and unload (`same_damage`: a cell with unsaved
damage never unloads), taken out of the scene on unload, and written with
the cell (a cell with only damage keeps its file; cleared, the file goes).

Rotations read back exactly when already unit (normalized again they would
move by a rounding, and a record would no longer be what was kept).

Cost: applying a record is the same work as making the placement (its
pieces counted against the sync budget). `keep_damage` walks the broken
placements and the forest's fallen trees once; follow_kept walks the
forest's cells and the scene's damaged cells only when the damage changed.

Known limits: `clear_damage` leaves kept craters (they are ground); a
record's placement deleted keeps its record (harmless: it applies to
nothing) until `clear_damage`; a kept piece has no body (the player walks
through rubble, as through far rubble); a kit's pieces changed since make
the record's piece indices the new kit's.

## Animation (N5 part 1)

`anim` (core only) holds skeletons and clips as plain data and the work
done on them each frame, so the importer, the editor and later the
simulation share one definition and none of it needs a GPU.

- **Skeleton.** Joints with names, parents (always before their children,
  so one pass from the start works out every joint), a rest pose as
  translation, rotation and scale, and inverse bind matrices; `root`
  places the root joints' parent. Model space is the file's own space.
- **Clips.** Per joint channels of translation, rotation or scale, each
  linear (rotations along the shorter arc at constant speed), step or
  cubic (glTF's Hermite spline, tangents scaled by the key spacing).
  `sample` starts from the rest pose and replaces what the clip animates,
  holding each channel's first and last keys outside them.
- **Skinning.** `model_matrices` then `skin_matrices` (model times inverse
  bind) per joint; a vertex is the weighted sum of its (at most four)
  joints' skin matrices applied to its bind-space place, weights in 255ths
  summing to 255. `skin_point` is the CPU reference the GPU pass will
  match. `pose_bounds` bounds a pose from the joints alone: each joint's
  vertices lie within the reach the import measured from it when bound,
  scaled by its skin matrix, so no vertex is visited per frame.
- **Player.** `AnimationPlayer` plays one clip (looping or once, at a
  speed, paused), cross-fades to the next over a time, or holds two clips
  blended (a walk and a run), the second one's phase following the
  first's when asked. It owns nothing but the clip numbers and times, and
  `PoseScratch` keeps evaluation free of allocation once grown.

The glTF importer reads one skin per file (a second is refused). Its
skeleton is the skin's joints plus any node between two of them (their
transforms count, as glTF says), ordered parents first; JOINTS/WEIGHTS
sets 0 and 1 are merged and cut to the four heaviest. Skinned meshes are
kept in bind space, as written (their node's transform is ignored, as
glTF asks); meshes that are not skinned but hang under a joint move
rigidly with it, others with the first root joint. `ImportResult::mesh` is
the character as its rest pose has it, so everything that draws meshes
unanimated (kits, scatter, the bind-pose preview) keeps working.
Validation refuses joint indices out of range, mismatched inverse binds,
key times that do not increase, values that do not match their keys,
clips over an hour, more than 1024 joints and more than 8 million keys.

Drawing them (`render`). A skinned mesh is built by
`geometry::build_skinned_clusters`: its original triangles alone, one
level, every group terminal (a pose moves the vertices, so errors measured
at bind would not hold), vertices merged only where their weights match
too. Each character adds its own (`Renderer::add_skinned_mesh`: every
instance of one shows its one pose), and it goes through the same cluster
path as everything else:
its groups stream into the geometry pool as terminal groups do (loaded
first, never evicted), and a source buffer keeps its vertices at bind
with their joints and weights, cluster by cluster in the pool's order.
`set_pose` takes one skin matrix per joint and a sphere around the pose;
in the frame after, before anything reads geometry, `skin.slang` (a
thread group per cluster) writes the posed vertices and normals into the
mesh's place in the pool and refits each cluster's culling sphere to
them, so culling, raster, shadows and shading read posed geometry with no
change of their own. It runs only for a new pose (or once the mesh has
loaded), not every frame. Instance culling uses the pose's sphere; the
CPU cut (GPU culling off) tests that sphere and then takes every cluster.
Shadow pages under a skinned mesh's instances are dirtied when its pose
or place changes. Skinned meshes are never left to the far field, and the
voxel world does not hold them (they bounce no light yet: a known gap,
until a coarse pose is voxelized). Later: levels of detail for skinned
meshes (refitting group bounds too) and morph targets.

Playing them (editor). `MeshLibrary` keeps a skinned file's skeleton,
clips and bind-space clusters beside its rest-pose mesh, which every
entity drawing it shows until it animates. An entity animates when it
draws a skinned file and has something to play: its saved
`Entity::animation` (clip, speed, loop; `set_animation`, undoable, saved
in scene and cell files) or a clip a command plays in the view
(`play_animation`, `blend_animations`: view state, until `set_animation`,
the entity hidden or drawing another file, or a new scene). `AnimationState` follows the scene's
changes (its own copy of them, kept by the scene frame beside the
physics', and what the scene has not handed out yet just before the scene
frame is built, so no cell is built with a copy it should not draw), gives each animated entity its own copy of the character in the
renderer (a few a frame, so loading many never stalls a frame), moves
their `AnimationPlayer`s on by the frame's time (its own `animation`
phase, after physics; a clip started this frame starts at its start),
works out the poses on two workers (`parallel_for`) and hands the renderer
only poses that differ from the last one sent (a same pose would redraw
its shadows for nothing). The
scene frame draws an entity's copy instead of the file's mesh once it is
loaded and posed. At most 64 animate at once: those animating keep their
place, a free one goes to the lowest id waiting, and the rest stand frozen
in the rest pose.

## Ballistics (W1a)

`sim::Ballistics` flies bullets through the physics world as point masses:
fixed steps of 1/240 s (the same flight at any frame rate), quadratic drag
(exact over a step for drag alone) then gravity, and one ray along each
step's chord. What a ray meets is a `world::Surface` (on `Material`: an
entity's, a kit piece's; the editor calls trees wood, characters flesh and
the ground's tiles dirt), and each surface has a resistance (joules a metre),
a ricochet angle and the share of speed a ricochet keeps (text-tunable,
`set_ballistics`). Under the ricochet angle it is mirrored off the surface;
otherwise its energy says how far it could go in (`reach = energy /
resistance`, at most 4 m), and a ray cast back at the same body's same part
(`RayFilter::body`, `part`: a Jolt shape filter on the compound's children)
from that far in finds the far side: found, it comes out there with the
energy left and meets what is next (another part of the same building is a
layer of its own, with its own surface); not found (it would end inside the
part, which a convex shape reports as a hit at once, or the terrain's height
field, which has no far side) it stops. A far side as near as the entry is
the entry seen from behind and does not count. A ray that starts inside
something (met at distance 0, or on a face it is leaving) never ricochets.
One step's ray goes through at most 8 surfaces, at most 4096 bullets fly and
a step() call casts about 16384 rays at most (fixed steps past that wait).
`target_of` names what each ray met as it happens (the entity and kit
piece), so hits applied after a piece is shot out still know what they
struck. Every meeting is a
`BulletHit` (speeds in and out, energy left there, the blow given), which
the editor turns into effects: dynamic bodies pushed by the momentum lost
(`PhysicsWorld::add_impulse`), and a breakable kit's piece worn
(`ScenePhysics::strike`: `blow_per_joule` of the energy, adding up to the
kit's strength, then shot out with what it held up; a frame's hits loosen
each placement once). `ScenePhysics::owner_of`
names the entity and kit piece behind a body and its part (`RayHit::part`).
Bullets fly with the simulation (after its step, so they meet the bodies
where they are now) and are cleared when it stops.

Characters (W1b) are met by their bones, not a body in the physics world:
at import `anim::fit_capsules` fits a capsule round the vertices each joint
moves most (in the joint's space: along the way to its children, or to its
vertices for an end joint; from the 2nd to the 98th percentile along it, as
thick as 70% of them lie from it). Each frame after the animation,
`editor::HitTargets` poses the capsules of every visible skinned entity
whose pose or placement changed (the animation's model matrices, or the rest
pose; `AnimationState::skinned` and `pose_serial` say what changed) into a
`sim::HitShapes`, a plain list beside the physics world (remaking Jolt
bodies every frame would cost far more). Bullets test it along each step
with an analytic ray-capsule test (the union of two balls and a cylinder:
in where it first enters one, out where it last leaves one) behind a
bounding sphere per character, and the nearer of it and the physics ray is
met. A character is one solid: its capsules overlap at the joints, so the
way out is where the ray leaves the last capsule it is still inside of (one
hit, named by the capsule it went in by). Only what changed is posed again
(nothing skinned changed: `HitTargets::update` returns at once), and an
animated entity is met in its pose once its copy is drawn (the rest pose
until then, as shown). Decor (`body` none) is not hit. Known limit:
`HitShapes::raycast` looks at every character's bounding sphere per ray (a
grid comes with crowds). Zones are named by rules on bone names
(`sim::HitZoneMap`, per file). Bullets meet the pose of the frame before
(the animation runs after the physics step).

## Gameplay rules (G1 part 1)

`gameplay` holds the rules of the game being built (FOLD, see
`docs/game-plan.md`): what items there are and what happens to them. It is
plain C++ data and functions with no GPU state, tested in ctest; the
engine decides how things move, render and get hit, `gameplay` what the
numbers are and what they mean.

- **Items** (`items.h`). An `ItemCatalog` holds every kind of item once
  (`ItemDef`: a stable id, a name, a kind, weight, base value, how many
  stack, whether it is worn, that is, has a condition from 0 to 1). It is
  loaded from text files (`"format": "giant-items"`), several into one
  catalog, all or nothing per file; an unknown field or a key given twice
  is an error, so a typo never passes as a default. Item indices follow the
  order of loading, so the game loads its files sorted by name; files and
  saves use ids. Numbers are written as the shortest decimal that reads back
  the same (0.73, not 0.7300000190734863), so files stay diffable. The game's files are `game/fold/data/*.items.json`
  and the tests load every one.
- **Inventories** (`inventory.h`). A backpack, a body, a crate or a stash
  is a list of stacks: an item, a count, a condition. Stackable items fill
  their stacks up to `max_stack`; worn items never stack, each keeps its own
  condition. Each stack has an id unique in its inventory and never reused
  (saves keep the next id too), so the UI and later rules can point at one
  stack. `set_condition` is the hook for repairs and wear. `remove`
  takes the worst condition first; `move` carries a stack's condition
  across. Weight is no hard limit, as in GAMMA: `burden` says whether the
  load is light, heavy or overloaded, and the player controller will slow
  down by it. Saved as text (`"format": "giant-inventory"`), items by id.

## Wind (E1 part 1)

`giant/core/wind.h` is the wind and how things move in it, shared by the
renderer, the editor and (later) the simulation. `world::ground_wind`
takes it from the weather: half the clouds' wind (the ground slows it),
gustiness 0.2 plus a fiftieth of the clouds' speed (a storm gusts at 0.8).
`WindClock` moves it on each frame (the editor's `WeatherState::wind`,
handed over as `FrameInput::wind`; without a weather, the default
weather's breeze): the gust pattern drifts at 0.8 of the wind and the sway
and flutter clocks run faster as it blows harder. Gusts are a sum of three
waves over a 64 m tile; the clocks wrap every 256 turns, and every
frequency used is a whole number of turns in that (rocking rates in
sixteenths), so nothing jumps when they wrap and floats keep their
precision for ever. The shader works camera-relative with the camera's
place modulo 64 m (`FrameConstants::wind_camera`); every phase is a whole
number of waves per 64 m, so the pattern at a place is the same from any
origin.

A thing sways when its material has `sway` or `flutter` (0 to 4 each,
`Material` in world and render). Its origin is its foot; `MeshEntry::top`
(the mesh's highest vertex) times its scale is its height. A point at
height fraction h moves along the wind by `height * h^2 * lean` (the lean
grows with the gusted speed and its square, rocks about itself at a rate
that falls with height, and is held to 0.3), a little across it, and drops
so it bends rather than stretches; `flutter` adds a small fast shake that
grows from the trunk outwards (leaves at the tips). `sway_offset` (C++) and
`wind_offset` (common.slang) are the same function, line for line;
`max_sway_offset`/`wind_reach` bound it. raster.slang moves each vertex as
it draws it and resolve.slang moves the three corners it finds the hit on
the same way, so the visibility buffer and shading agree; normals stay as
they rest (the bend is gentle). Culling (instances, clusters, the retest,
and the CPU's cut) grows each swaying instance's spheres by its reach.
Skinned meshes never sway (their `top` is 0).

Sway fades out over the last third of `RenderSettings::wind_distance`
(150 m), so the far field and the voxel world, which hold things at rest,
match what stands still out there, and cached shadow pages far off stay
valid. Near the camera, the renderer gathers swaying instances into 32 m
squares each frame (`sway_shadows`) and `ShadowPages::sway` marks the
pages under them on the levels whose texels are finer than the motion;
`plan` draws up to `wind_shadow_pages_per_frame` (32) of those again a
frame, longest drawn first, besides the usual budget, so the shadows sway
too, at a lower rate when many pages do. Known limits: that gathering
walks the frame's instances (a byte per material says which sway, so still
ones cost a load and a test); a kit's pieces each sway about their own
origin, so a tree should be one mesh with its foot at its origin.

## Cloth (E1 part 2)

`world::Shape::cloth` hangs a sheet from its entity's origin, in the
entity's x-y plane (`ClothSettings`: `width` across, `height` down, `pin`
top edge, top corners or left edge, `weight` per square metre,
`stiffness`); kit pieces and scatter items cannot be cloth, and the
entity's scale is unused. `giant/sim/cloth.h` moves it: a grid of points
about 0.15 m apart (3 to 32 a side, so at most 1024: the skinned meshes'
joint limit) joined by springs along the grid, across each square and over
two points (softer as `stiffness` falls), with position-based dynamics:
a Verlet step under gravity and the wind, then the springs' lengths put
back 6 times. Each sheet is simulated in its own space (wind and gravity
turned into it), so moving its entity moves it whole. The wind is
`wind_at` where it hangs (the gusts the trees show); it pushes each point
along its normal with the relative wind's normal part squared (a flat
plate), a little along its face, and in ripples that run away from what
holds it on the flutter clock, so a storm throws it far harder and faster
than a breeze and heavier cloth moves less. No point moves more than half
the grid's spacing a step; after the springs, tethers keep each free
point within 1.05 times its rest distance of the pins holding it (its
column's top, its row's pole, or both corners) and a last pass shortens
any spring over 1.1 times its length, so a 60 m/s storm stretches a 4 by
6 m sheet at most about 1.4 times between neighbours (blowing along its
face; about 1.1 face-on). A sheet whose numbers stop being finite is put
back at rest (`resets`).

Work is bounded (rule 9): the nearest `max_simulated` (32) sheets within
`distance` (60 m) of the camera move, at most `max_points` (8192) points
between them (the nearest always), in fixed 1/60 s steps, at most 4 a
frame; the rest hold their last pose. A sheet that has moved less than
0.02 mm a step for a second with no wind sleeps until wind, a push or its
entity moves it. 32 banners 1 by 2 m (112 points each) cost about 1 ms of
one core a frame; the editor spreads sheets over three workers.

The editor's `ClothState` follows the scene's changes before the scene
frame is built (as the animation does), makes each sheet its own skinned
mesh (`cloth_mesh`: the grid's points at rest, front and back faces, each
vertex weighted wholly to its point, so the joints are the points; at most
256, a few a frame) and, after the weather, steps the sheets and hands
the renderer the pose of each that moved: per point, a matrix from where
it rests to where it is, turned to its patch's frame (along the grid and
its normal). The renderer draws it as any skinned mesh: posed on the GPU,
culling spheres refit, shadows drawn again where it moved; it is never
left to the far field and the voxel world does not hold it. A cloth entity
draws nothing until its mesh is loaded; one given a new size (a drag in
the panel) keeps its old mesh, still, until the new one is loaded and
swapped in before the next cells are built, and a mesh the renderer
refuses is tried again only when its entity changes. `get_cloth` and
`push_cloth` follow the scene's latest edits first. `Cloths::push` (the `push_cloth`
command) gives every point near a place a change of speed, less towards
the edge; `Cloths::blast` throws the points away from a place, the blow's
impulse over the sheet's weight (at most 30 m/s): the `impact` command
does that (E1 part 3, below). Known limits: cloth passes through what it
hangs against (no collision yet), bullets pass through it untouched, and a
far sheet holds still rather than waving.

## Swinging things (E1 part 3)

Lanterns, signs, bells, lamps on chains: any cube, sphere, plane, mesh or
kit entity with `SwingSettings::on` hangs from a pivot `length` metres
straight above its origin and swings about it (`hinge` free, any way; or
`x`, only about its own x axis made level: a sign on two hooks), at most
`limit` degrees from hanging straight, weighing `weight` kg. Saved with
the entity (only when set); kit pieces and scatter items do not swing
(the kit entity swings whole).

`giant/sim/swing.h` moves them: each is a pendulum, its origin on a rod
from the pivot. Each fixed step (1/60 s, at most 4 a frame) gravity pulls
it, the wind where it hangs (`wind_at`, so the gusts the trees show)
pushes on its box as it is turned now (`size`: the area it shows the
relative wind, a box's drag, growing with the speed squared, never past
the wind's own speed in a step: a light thing in a storm stays stable),
its pivot slows it, then the rod puts it back (its length, the hinge's
plane, the limit) and its speed is what the rod let it do. So a storm
leans a lantern far over and throws it about in the gusts, a breeze barely
stirs it, and a sign swings only across its face. Shots (`knock`: the
bullet's momentum lost in it, over its weight, at most 20 m/s), pushes and
blasts (away from the blast, the impulse over its weight) change its
speed. Work is bounded (rule 9): the nearest `max_simulated` (512) within
`distance` (100 m) move; the rest hold their pose. One that moved under
0.02 mm a step for a second sleeps until the wind about it changes (by
0.05 m/s) or something knocks it, so steady wind costs nothing.

They have no meshes of their own: each is drawn turned about its pivot.
The editor's `SwingState` follows the scene's changes (as the cloth
does; the box is the entity's scale for primitives, its pieces' bounds
for a kit, its mesh's size once loaded), steps after the weather and
lists those whose pose moved more than 0.1 mm. `scene_frame` takes those
(and the physics' moved bodies) as `moved`: each cell keeps what each
entity drew (`Slot`: its instances in the cell), so only the moved
entities' instances are written again in place and the renderer is told
of that range alone (`CellRange::prev_version`, joined with the cell's
ground change in place that frame, if any; decals moved with them), and
the instance store uploads just that range at the cell's kept anchor; a
cell with anything else changed, or an entity drawing other than it did
(a piece's mesh not loaded), is rebuilt whole as before. The nearest four
times `max_simulated` are looked at a frame (sleepers waking), the rest
wait. Swinging entities' instances are marked
`render::Instance::moves`: the voxel world does not hold them (no brick
work as they move; they bounce no light, as skinned meshes, even far ones
holding still) and the far field never stands in for them. Their shadows move with them (the shadow
pages under moved instances are drawn again). While the physics poses an
entity (a dynamic body, simulating), its pose wins over the swing's.

`PhysicsState::on_hits` and `on_impact` (set by `connect_blows`) carry the
shots' hits and the `impact` command's blows out: a hit on a swinging
entity knocks it; a blow throws the swings and cloth in reach. The
commands `get_swing` and `push_swing` follow the scene's latest edits
first. Known limits: a swing does not collide with what it hangs by (the
wall behind a lantern), it does not spin about its rod, its physics body
(if any) stays where it hangs at rest, and chains are one rigid rod.

## Navigation

`giant/nav/nav.h` says where people can walk and finds the way. The
walkable ground is a grid per world cell (64 m): 128 x 128 samples 0.5 m
apart, each with the height one stands at and whether one can. A sample is
walkable where the ground is no steeper than `max_slope_degrees` (40),
nothing is in the way from `step_height` (0.4 m) above it up to
`clearance` (1.8 m), and it is `agent_radius` (0.35 m) from what is
(obstacles, oriented boxes, grow by it). Low things (a kerb, a slab) are
stood on: their top is the height there. Where there is no terrain, the
ground is the lowest obstacle top with no lid too low over it (a floor
plane, a deck, not a footing under the floor). `build_tile` is pure (any
thread): about 1.6 ms a tile, sampling the terrain once on a grid a sample
wider than the tile for the slopes at its edges.

`PathSearch` is A* over the samples of the tiles there are (8 ways, no
step higher than `step_height`, no corner cut past a blocked sample), in
slices of `advance(nodes)` (rule 9: about 0.45 us a sample, so a frame's
1536 cost about 0.7 ms), from and to the nearest walkable samples within
`snap_distance`. What it knows of each sample is kept densely per tile.
Reaching a tile not built it stops in `missing` (that cell): build it and
advance again. A path found is pulled straight where the way is clear
(corners only where it turns). It gives up at `max_nodes` samples (about
0.8 km of open ground across); a hierarchy of tiles for longer ways is
planned.

The editor's `NavState` (editor/src/nav_state.h) keeps the map for the
scene: it follows the scene's changes (each fixed entity's boxes, shown or
not, as the physics collides with hidden ones:
cubes, spheres, planes as a slab under their top, meshes as a box inside
their bounding sphere, kit pieces not knocked loose; scatter trees'
trunks) by cell, and a built tile's inputs (the terrain's tile versions and
the scatter serials around it, its cell's entity stamp) are checked a few a
frame: tiles whose inputs changed are built again, two at a time on its own
two workers, from copies, so the scene changes freely meanwhile. Searches
(`find_path`) move on within `nodes_per_frame` a frame, asking for the
tiles they wait for first; the oldest unused tiles go past `max_tiles`
(512, about 40 MB; a rebuild is not a use). A search found over a tile
older than what it is built from is searched again once that tile is
built. A search that read a tile since rebuilt or dropped starts again
(`PathSearch` checks the versions of the tiles it read; at most 16
times, then it fails), and making a
found path (walking back, pulling straight) is spread over `advance`
calls like the search. A kit or mesh change looks at the entities using
it again, 256 a frame; a new scene drops every tile. The `find_path`
command builds what it needs now (bounded), rebuilding a stale tile its
path crosses and searching again (`stale` says one was left).
Dynamic bodies, swinging things, animated meshes, cloth and decals are not
in the map: what moves is avoided by what walks, not by the map. Known
limits: scatter trunks count only where the scatter is placed (within its
radius of the camera), so a far path ignores forests and tiles there are
built again as the camera nears; an entity covering more than 64 cells (a
floor under a town) makes every built tile stale when it changes; felled
scatter trees still block until their cell is placed again;
a mesh is a box (not its shape); one storey a sample where there is
terrain (a bridge over ground is stood on only if within a step).

### Walkers

`giant/nav/follow.h`'s `PathFollower` steers along a found path: towards
the next corner (turning for the one after within 0.6 m, or once nearer
it), slowing over the last 1.2 m and stopping within 0.25 m of the end.
Others near (the nearest 8 within 1.5 m past touching) push it away, the
closer the harder, and one ahead coming at it makes it step to its right,
so two meeting pass left side to left side. Pure: the caller moves it.

The editor's `WalkerState` (editor/src/walker_state.h) makes an entity
sent walking (`walk_to`, body `none`, while simulating) a physics
character, steers each before the physics steps (others found by 2 m
squares) and poses each after (between the last two fixed steps, turned
to face where it goes). Poses go to the scene's drawing with the swings'
(moved in place, kept out of the voxel world); the scene keeps where it
was put. `walk_to` finds the way at once when it takes at most 64 tiles,
else NavState searches over the next frames. A walker moving under 0.3 m
in 2 s looks for the way again (four searches in all, then it has
failed). Stopping the simulation or another scene lets every walker go;
`step_physics` steers them before each fixed step. Known limits: a
walker is drawn in the cell the scene puts it in (one walking far may be
culled with that cell); no animation is chosen for its pace yet; a stuck
walker's new search waits for frames (not step_physics).

## Window events (known limit)

`Window::poll_events` drains SDL's single, process-wide event queue and drops
events for other windows. That is fine while the editor has one window. Before
a second OS window exists (floating panels, a game preview), events move to one
central pump in `platform` that hands each window its own events.

## The shared voxel world

One sparse voxel copy of the world (brick map in camera-centred clipmaps) is
used for far geometry, global illumination, rain and snow occlusion, NPC sight
lines and far-ring destruction. It is owned by `world` (edits) and mirrored on
the GPU by `render` (drawing and lighting).

Done in N2 part 2:

- `engine/voxel` holds the plain-data pieces: bricks of 8x8x8 voxels (an
  occupancy bit per voxel, then colour and normal packed in one u32, then a
  stand-in bit per voxel (part 3); 2176 bytes, the GPU's layout too), a conservative triangle-box test to
  voxelize surfaces without holes, and the clipmap. Level 0 has 0.25 m
  voxels; each of the 11 levels doubles them, so level 10 (256 m voxels)
  reaches about 30 km out. Each level is a window of 32 bricks per axis,
  moved in whole bricks once the camera is more than a brick from its
  centre; a brick's slot in its level's table is its coordinate modulo 32,
  so moving only fills the slabs that came into view.
- `render` builds it (src/voxel_scene.h) from the frame's instances,
  materials and cluster meshes: per brick, each instance over it adds the
  clusters of a LOD cut at half a voxel of error (so a dense mesh voxelizes
  from few triangles at coarse levels), skipping clusters whose bounds miss
  the brick. It keeps its own copy of the instances (bounds from the
  meshes' cluster spheres, resolved colours) in a 32 m grid, and each frame
  compares the frame's instances with it. A moved, changed, added or
  removed instance (one contiguous run removed counts as removals) rebuilds
  the bricks under where it was and is; a recoloured material or an added
  or removed mesh counts as a change to the instances using it. Changes
  wait gathered by 64 m square (a box wider than two squares waits on its
  own): each square becomes one job per level over its boxes' bricks (the
  old boxes' built bricks, every brick under the changed instances), so
  thousands of scatter placements arriving are a few hundred jobs, not a
  rebuild. More than 256 changes in a frame (or an eighth of the
  instances), over 4,096 squares waiting, or any other removal rebuild
  every window, one rebuild at a time (another waits for it rather than
  restarting it, so constant changes still settle); `voxels.rebuilds`
  counts them. Big searches (coarse bricks, whole windows) look through
  the grid's 512 m coarse cells, not every 32 m cell.
- All the work is queues worked through under
  `RenderSettings::voxel_ms_per_frame` (1 ms, at least one step and one
  brick a frame): finding bricks in resumable steps (a time check every
  256 marks), then building, coarse levels first, a few instances at a
  time, so a coarse brick over thousands of instances spreads over frames.
  Until a brick is rebuilt the old one stays; a slab that just came into
  view reads "unbuilt" and the next coarser level stands in. Bricks live in
  a fixed GPU pool (`RendererDesc::voxel_bricks`, 32768 = 68 MiB) with one
  u32 slot table per level (only the changed range is copied, with no CPU
  wait); bricks that do not fit wait for room (`missing`). `probe_voxels`
  reads a CPU copy of the occupancy. The gate scene (town and 10,000
  trees: 20,000 instances) takes about 7,700 bricks and some 2.3 s of CPU
  time over about 2,100 frames (the coarsest bricks lie under every
  instance); in release builds its frames stay near the
  1 ms budget, at worst a few ms (a search over a whole window, or the
  start of a coarse brick, is not split), even with every tree moving each
  frame, except the first (copying the instances and opening every
  window, about 7 ms; a teleport costs the same).
- Known costs left: comparing the instances is a pass over all of them each
  frame (about 0.2 ms for 20,000), and a full rebuild copies them all in one
  frame; world streaming (N3) will hand over changes instead.
- The voxels debug view marches each pixel's ray through the levels,
  skipping empty space with per-level bounds and empty occupancy words.
- Not yet: `world` owning edits (destruction carving it, N4); building on
  worker threads (with cell streaming, N3); textures (colour is the
  material's base colour); GI and reflections that read it (part 5).

Done in N2 part 3, the far field:

- Past `RenderSettings::far_field_distance` (3 km by default) the voxel
  world stands in for small instances: the CPU leaves out every instance
  whose bounding sphere (radius at most `far_field_max_radius`, 50 m) lies
  wholly beyond the distance and that the voxel world already holds, so
  they cost no culling, streaming or raster work at all. Larger instances
  (terrain, mountains, big buildings) stay meshes at every distance,
  cluster LOD keeping them cheap. `RenderStats::far_culled` counts the
  ones left out.
- Voxels remember what they stand in for: voxels touched by an instance
  of at most `far_field_max_radius` (a stand-in) carry a stand-in bit, and
  take that instance's colour and facing over the big surfaces sharing the
  voxel (a far tree stays green over the ground it stands on). Changing
  the radius rebuilds the voxel world.
- The shading pass (`shaders/resolve.slang`) marches the voxels for every
  pixel whose mesh hit (if any) is past the distance, from the distance up
  to that hit, with the march the voxel view uses
  (`shaders/voxel_march.slang`), seeing only stand-in voxels. So meshes
  still drawn (terrain, mountains, buildings) are never covered or fattened
  by their own voxels, and small things left out show on top of them. The
  voxel's colour and normal are lit like a mesh (a rough dielectric). A
  stand-in voxel is up to a voxel larger than its object, so it can poke
  out in front of a mesh just before it. Where a stand-in is still drawn as
  a mesh (straddling the distance, moving, or not in the voxel world yet)
  the march ignores the voxels within two voxels of its bounding sphere,
  so its own voxels never cover it while others in front of it still
  show (other than ones that close to it: a far tree just before a moving
  car shows the car there).
- The march is kept cheap: levels flag whether they (or a coarser level
  standing in for their unbuilt slots) have stand-in bricks, so with none
  a ray skips the level, and with none anywhere there is no march at all;
  a brick without stand-in voxels is crossed in one step.
- Lit view only, and only while the voxel world is kept up to date (a
  frozen one would show stale far geometry). The levels are written once a
  frame for the far field and the voxel view.
- How sharp it is: each level window is 256 voxels across, so wherever a
  level takes over its voxels are about 7 to 14 pixels wide at 1080p
  (60 degree view). A far tree reads as a blob of a voxel or two; finer
  far detail needs more bricks per axis (`RendererDesc::voxel_clipmap`,
  64 halves the voxel width for some four times the bricks) or the
  proxies of part 5 and N3.
- An instance is left out only once the voxel world holds it as it is
  (`VoxelScene::holds`): every brick under it, on every level, built from
  it, none waiting to be found, rebuilt or for room in a full pool. So a
  scene still building, an instance just added or moved far out, a slab
  just come into view or a full pool keep meshes, never holes. Checking
  costs a few lookups per far instance and frame.
- Not yet: fading between mesh and voxels
  at the distance (the switch can show). A stand-in still drawn as a mesh
  also has its voxels around it (a halo up to a voxel wide against what
  is behind), and one that moves leaves them behind for a few frames until
  rebuilt; something far that changes every frame (a vehicle) is never
  left out and shows both. Stand-ins could drop their bit while they keep
  changing.

## Sun shadows

Done in N2 part 4: virtual shadow maps for the sun (`src/shadow_pages.h`,
`shaders/shadow_pages.slang`), drawn from the same clusters and culling as
the view, so every mesh casts at the detail its shadow texels need.

- The sun's view is a clipmap of 12 square levels in light space, centred
  on the camera: level 0 is 16 m across, each level twice the one before
  (level 11 is about 33 km). Each level is a window of 16 x 16 virtual
  pages of 128 x 128 texels, so texels run from 8 mm near the camera to
  16 m at the edge. A window moves once the camera is more than a page
  from its middle (so a camera bobbing on a page edge does not move it
  back and forth); a page keeps its slot (its coordinate modulo 16) while
  it stays in the window.
- Depth spans 40 km along the light, measured from a fixed anchor near
  the camera, not the camera itself, so a cached page stays right while
  the camera moves; the anchor moves (and every page is drawn again) only
  once the camera is 10 km from it along the light.
- Only pages something on screen needs are kept. Shading picks, per pixel,
  the finest level whose window holds the point with room for the filter
  and whose texels are no finer than the pixel; it asks for that page with
  a bit in a request buffer (and, while the page is not drawn yet, for
  each coarser page it falls back to, until one is drawn, and for the
  level above the one it uses, so a fallback is there when a new sun makes
  pages draw again, coarse levels first). The bits are
  read back two frames later, with the windows they were made in: a slot
  whose page has changed since is dropped. Each page asked for gets room
  in the pool (1024 pages, a 64 MiB atlas of u32 depths,
  `RendererDesc::shadow_pages`, at most 8192). When the pool is full the
  page asked for longest ago makes room, never one asked for this time;
  pages that leave their window are freed.
- A drawn page is cached until what casts onto it changes. Each frame the
  CPU compares the instances with its copy (position, rotation, scale,
  mesh, and whether it is drawn as a mesh this frame: one left to the far
  field casts nothing) and dirties the pages under the old and new bounds
  of each that changed, on every level (a shadow lies under its caster
  across the light). Dirtied boxes are gathered by 32 m square (a wider
  box keeps its own), and many squares at once (over max(256, an eighth
  of the instances)) dirty everything. The copy is kept by index, so removing an instance
  early in the list reads as all the rest moving (everything is redrawn).
- A new sun direction (taken up in steps of a quarter of a degree, so
  time of day redraws now and then, not every frame) makes every page
  read as missing until drawn again: light space turns about the world's
  origin, so old pages are of no use. Coarse levels are drawn first (and
  pages not drawn are drawn whether or not they were asked for lately),
  so for a few frames the shadows are coarser, not gone. Shadows follow the
  stepped sun, so they may lag the lighting's sun by up to a quarter of a
  degree (a 100 m tower's shadow by about 0.4 m).
- Drawing is budgeted: at most `shadow_pages_per_frame` pages a frame (64
  by default, at least one when any wait), drawn pages only if asked for
  in the last 30 frames: pages never drawn first (coarse levels first), then the
  pages dirty longest, so every level gets its turn however often things
  change (a giant walking through dirties pages on every level each
  frame). A dirty page keeps its old content until redrawn, so a moving
  caster's shadow may lag a few frames when many pages wait. Right after
  the camera jumps (or a new scene) pages are missing for the few frames
  their requests take, and shading falls back to coarser ones or lit.
- Per level with pages to draw: the pages are cleared, then culled and
  drawn like the view's first pass (no occlusion) with their own lists:
  the frustum is the pages' rectangle across the light plus a plane half
  the depth range towards the sun; LOD picks clusters by their error in
  shadow texels. The raster pass has no targets (the graphics layer's
  `RenderPassDesc::width`/`height`), is scissored to the rectangle and its
  pixel shader (`raster.slang` `ps_shadow`) keeps, with an atomic max, the
  depth nearest the sun in the page's place in the atlas, only for pages
  being drawn this frame. Both faces cast. Shadow culling marks the
  groups it uses (they stay loaded) but asks streaming for nothing: it
  draws what is loaded.
- Shading offsets the point along its normal by 1.5 texels, adds a depth
  margin of a texel (more where the sun grazes the surface) and filters 3
  x 3 texels with bilinear weights (16 taps; a tap on a page not drawn
  does not count). Points past the far end of the depth range are lit.
  Page requests are made once per wave where the whole wave wants one
  page. Far voxels are shadowed through the same lookup.
- Lit view with GPU culling only (the CPU path and the debug views are
  unshadowed). `RenderStats` (and `last_frame.shadows` over MCP) report
  the pages resident and drawn, those waiting, those asked for that found
  no room (as of the newest requests), and the raster passes. The work is
  timed as "shadows".
- Not yet: casters left to the far field (small things past 3 km) cast
  nothing; finer geometry streaming in does not redraw the pages it falls
  on; a shadow culling list that overflows leaves holes in its pages
  until they are next drawn; no contact-hardening (soft shadows by
  distance) or local lights.

## Global illumination

Light bouncing off the scene, and the sky's light shut out by it, from the
lit voxel world (N2 part 5; `src/voxel_light.h`, `shaders/voxel_light.slang`,
`shaders/voxel_gi.slang`). No ray tracing hardware, no noise to denoise and
nothing carried over from earlier frames, so the image is as sharp moving
as standing still.

- Every filled voxel has a light word in the light pool, a buffer beside
  the brick pool in the same pool indices (one u32 per voxel, 2 KiB a
  brick, 64 MiB for the default 32768 bricks, made on the first frame with
  GI): the bounce and sky light reaching it (RGB, 9-bit mantissas over a
  shared exponent, `voxel::pack_light`) and a bit for whether the sun
  reaches it. A word of 0 means "not lit yet".
- The GPU lights up to `gi_bricks_per_frame` bricks a frame (256 by
  default, at least one; one group per brick, its filled voxels shared out
  evenly). For each filled voxel: a march through the voxel world towards
  the sun (from its own level up) sets the sun bit, and six cones over the
  hemisphere around its normal (one straight out, five at 60 degrees,
  cosine weighted, 30 degree half angle) gather the light. A cone steps
  out through the clipmap, sampling the level whose voxels are about half
  its width (the coarser levels serve as its wider samples), each filled
  voxel taking 85% of what is left; voxels straddling the surface the
  cone leaves (centre less than half a voxel above its plane, taken a
  voxel above the lit voxel's centre: a surface on a voxel boundary fills
  a voxel on each side) are left out, as coarse voxels are fuller than what they hold (the sun march
  leaves them out the same way). What a voxel sends back is lit as
  shading lights a Lambert surface: its colour times the sun over pi
  (when its bit says the sun reaches it) plus its own light word, so
  light bounces once more each time a brick is lit again. What
  gets through comes from the sky (the sky and ground colours by
  direction).
- `VoxelLight` picks the bricks on the CPU: a brick just put in a pool
  index is first cleared to "not lit" (what the index held was another
  brick's), then lit before anything else, as is a brick built again in
  place (it keeps its old light meanwhile); the rest are lit again in
  turn, round the pool, with a quarter of the budget always kept for the
  round (so it goes on while the camera flies and new bricks keep
  coming). So a moving sun or a change nearby reaches every brick within
  (bricks / budget) frames, about (4/3 x) that while new bricks keep
  coming (a few hundred frames for a full pool at the default). Turned off
  and on again (or after a debug view without it), the bricks put in the
  pool meanwhile are cleared (the voxel world numbers its placements);
  the rest keep their light until the round reaches them. If a frame's
  clears are lost (out of memory), the next frame clears them again.
- Shading looks the light up per pixel (`voxel_bounce`): on the finest
  level that holds the point, the 2 x 2 x 2 voxels around it (at the
  point itself, so a surface on a voxel boundary gets the same weight
  whichever way it faces), weighted by distance and by how far each faces
  the same way as the surface (a wall's other side, the floor under it, do
  not count). Where none there is lit, the next levels up; over the last
  brick before a window's edge it blends with the next level up, so the
  windows moving show no seams. The light replaces the flat sky and ground
  colours for diffuse, and dims the sky in reflections as much as the sky
  is hidden (for surfaces too rough for traced reflections). With nothing lit around (a
  brick just built, things the voxel world does not hold) it falls back to
  the flat colours. Far voxels use their own light word.
- Lit views (and the voxel view, which then shows voxels with their own
  light) with `voxel_world` on; the debug view `lighting` is the lit view
  in plain white-grey, the light alone. (The voxel view without GI keeps
  its simple, brighter sun term.) `RenderStats` (and
  `last_frame.gi` over MCP) report bricks lit and cleared this frame and
  those new or rebuilt still waiting. Timed as "gi".
- Known limits: the light is only as fine as the voxels (0.25 m near the
  camera, doubling outwards), so small gaps and contact shadows are left
  to later small-scale occlusion; walls thinner than a voxel leak light;
  coarse voxels are fuller than what they hold, so cones far out see a
  little too much blocked; emissive surfaces do not light the voxels yet;
  light lags changes by up to a round of the pool.

## Reflections

Glossy surfaces reflect along the mirror direction, traced every frame
from this frame's data alone (AGENTS.md rule 2: no history), so
reflections are as sharp moving as standing still.

- Voxel cones (`trace_reflection` in `voxel_gi.slang`, with `gi` on),
  in shading (`resolve.slang`): a cone through the lit voxel world along
  the mirror direction, as wide as the highlight (tan of the half angle
  from roughness squared, 0.03 to 0.577). Samples are at least a sixth of
  their distance wide and steps an eighth of it or more, so 48 samples
  reach a hundred metres and more. What they meet shines with its light
  word (as GI's cones do); the rest of the cone takes the hemisphere
  colours, dimmed by GI where the sky is hidden (so no sky indoors). The
  surface's own voxels are left out (a plane half a voxel above it), and
  with them coarse voxels further out holding its plane. Full weight up to
  roughness 0.45, none past 0.6; rougher surfaces keep the dimmed
  hemisphere.
- Screen space (`reflect.slang`, `screen_space_reflections`): shading
  writes radiance before exposure (rgba16 float), and for glossy pixels
  the reflection's weight (Fresnel times the roughness fades, 10:10:10)
  with the normal (octahedral, rg32 uint) and the reflected light it used
  (rgba16 float). A full-screen pass then marches each glossy pixel's
  mirror ray against the depth buffer (32 steps, closer together near the
  surface, out to 200 m, then 6 halvings; surfaces taken to be
  0.3 m + 2% of their distance thick); where it hits a lit surface, that
  pixel's radiance replaces the reflected light shading used. It fades out
  at the screen's edges, for rays coming back at the camera and from
  roughness 0.25 to 0.4 (mirror sharp: no blur yet). The same pass applies
  exposure and tone mapping (`post_draws` counts it); with screen
  reflections off, shading tone maps in place instead (`ps_direct`), one
  pass and one target as before.
- Targets: 24 bytes a pixel (about 88 MB at 1440p). `RenderStats::
  reflections` and `screen_reflections` (`last_frame.reflections.voxel`
  and `.screen` over MCP) say which are on.
- Known limits: reflections in mirrors show what shading gave the
  reflected point (a mirror in a mirror shows the voxel reflection, not a
  second sharp one); screen reflections are mirror sharp up to their
  roughness limit; voxel reflections are only as fine as the voxels they
  meet (coarse further out, and they lose the foot of distant things
  standing on the reflecting plane), miss emissive light and leak through
  thin walls; far voxels (the far field) do not reflect.

## Frame budget (1440p, 60 fps, reference GPU, High preset)

Frame pacing counts as much as the average (AGENTS.md rule 9). Performance
gates check the 99th percentile frame and count hitches (frames half again
over the target frame time). Where it stands:

- Done: no shader compiles at run time (shaders are built ahead) and every
  pipeline is created at load (`Renderer::init`, the device's UI pipeline).
  The C# GC runs in background, sustained low latency mode, and the script
  host does not allocate per frame.
- Done in N2 part 1: resources created with initial data inside a frame
  (the streamer growing its usage table, meshes added with
  `Renderer::add_mesh` mid-frame) are staged and copied on the GPU before
  the frame, with no CPU wait; terminal
  cluster groups load within the streamer's per-frame budget like
  everything else (a mesh draws once all of its are in, a few frames after
  many are added at once); the GPU times its passes, so budgets can be
  checked against measurements (`get_engine_info`, the Engine panel).
- Known gaps: the 32 MiB per frame streaming budget is set by hand, not yet
  tuned from those GPU timings; what callers create in one frame is not
  budgeted by the device, so a big batch of meshes added mid-frame uploads
  all their metadata (clusters, groups, LOD tree) in that frame (the
  world's cell streaming spreads it, N3); the streamer's zero-filled usage
  table, when it grows, is uploaded outside the streaming budget (rare: on
  doubling, and small); the editor's mesh import runs between frames, so
  its `add_mesh` still takes the load-time path that waits for the GPU, and
  it reads and clusters files on the main thread (editor only today; a
  streamed world imports every mesh it lists when it opens, so cells never
  import while streaming; moving imports onto jobs is still to come).
  World streaming (N3 part 3): unloading cells is one `Scene::remove_many`,
  a pass over the scene's entities (a few milliseconds at tens of
  thousands of entities, once every `unload_every` frames that unloads
  anything; entities should stay in the tens of thousands per loaded
  area, with scatter for the rest), and a
  cell joining or leaving relays out the instance list (see "World
  cells"). `save_world` and `open_world` are commands, not frames: they
  wait for reads in flight and read or write files on the main thread.
  Terrain (N3 part 4): `create_terrain` makes its heights between frames
  (spread over the terrain's workers; about half a second for 4096
  hilly tiles of 64 samples, the most a command may make), and a mesh
  that arrives for a cell with nothing else in it re-sorts the cell order
  (every frame of a first build). Terrain streaming (N3 part 4b): each
  tile set or freed costs about 6 to 20 us (64 to 128 samples) within
  the budgets above, and the two mesh workers build about 115 tiles of 64
  samples a second: on opening a world at the default radius the ground
  is all drawn after some 16 s, and flying faster than about 50 m/s at
  that radius outruns them (late full tiles, not stutter: the far ground
  draws until they are in). Far ground (N3 part 4c): about 800 level-1
  and 200 level-2 chunks at the default radii (4 KB of heights each),
  read on a worker and meshed on each level's two workers; what draws is
  worked out for changed chunks only (a level-2 chunk looks at its 64
  children, a level-1 chunk at its 64 cells: hash lookups).
  Physics (N4 part 1): at most four fixed steps a frame on physics' own
  workers (the main thread waits for them). A frame's sync looks at no
  more than 1024 entities, a body counting each of its parts (1024 cubes
  or 25 kit placements of 40 pieces, about a millisecond, made in one
  batch; an eighth kept for dynamic entities, which have a queue of their
  own, so an edit to one never waits behind a stream of fixed ones), and copies out no more than 16
  terrain tiles, whose height fields (0.2 to 0.8 ms each at 64 to 128
  samples) are made on a worker of their own (one thread, kept) and
  swapped in when ready, the old tile holding the ground meanwhile; the
  rest waits for the next frame, so opening a big world builds its bodies
  over a few seconds, not in one frame. A new dynamic entity's body waits
  until no fixed one is waiting and the ground round it is in, so nothing
  falls through. A new scene (new, load, open, and their undo) starts a
  fresh physics world (about 4 ms on the main thread, sized for 262,144
  bodies) and lets the old one's bodies and the follower's tables go on
  threads of their own (tens of milliseconds for 60,000 bodies, off the
  frame; at most two old worlds at once); with the follower's own
  clear-up and first scan that frame takes 10 to 20 ms in all for a
  50,000-entity scene on 4 cores (a scene swap: the renderer rebuilds
  everything that frame too), and the scene's entities are then looked
  at a share a frame. Every body
  that moved rebuilds its cell's instances that frame (as any edit does;
  cost grows with what else is in the cell). Kits are compared whole each
  frame (as the renderer does). `raycast` and `step_physics` finish
  whatever waits first (commands, not frames; `step_physics` of a minute
  runs 3600 steps, about half a second among 50,000 fixed bodies), and
  `get_physics` counts every body (a few milliseconds in a big world).

| GPU work | Budget |
| --- | --- |
| Geometry: culling, clusters, visibility buffer | 3.0 ms |
| Materials, decals, terrain | 3.0 ms |
| Shadows | 2.0 ms |
| GI and reflections | 3.5 ms |
| Weather, clouds, particles | 1.5 ms |
| Hair | 1.0 ms |
| Post and UI | 1.0 ms |
| Headroom for destruction spikes | 1.6 ms |
