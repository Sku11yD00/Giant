# Architecture

Five layers. Each layer only uses the layers below it.

```
game        C# scripts, levels, materials, scatter graphs, weather presets
editor      panels, play-in-editor, undo; uses everything below
ai          command layer, MCP server, rule checks
sim world   giants, destruction, NPCs, animation | cells, streaming, landscape, scatter, weather
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
| geometry | core |
| voxel | core |
| render | core, rhi, geometry, voxel |
| world | core, voxel |
| sim | core, world |
| script | core, world, sim |
| ai | core, platform, rhi, geometry, render, world, sim, script |
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
  drawn) rebuild everything. Instances go cell after cell (by z, then x;
  within a cell by id), and `FrameInput::cells` says where each cell starts
  and its version (new on every rebuild).
- **The renderer looks only at cells that changed.** The voxel world and
  sun shadows each keep a copy of the instances to find what moved; with
  cells laid out as when they last looked, they compare only the cells
  whose version differs (`src/cells.h`). Otherwise (no cells, a cell
  added, removed or resized) they compare everything, as before.
  `RenderStats::cells` and `cells_changed` count them.
- **Instances stay on the GPU (N3 part 2, `src/instance_store.h`).** One
  buffer keeps every instance, in frame order, from frame to frame; a frame
  uploads only the cells that changed (all of them for a new layout or
  material table size, or without cells). Translations are kept from each
  cell's anchor (its first instance when uploaded), which keeps them
  precise in float however big the world; each frame writes the anchors
  relative to the camera (16 bytes a cell) and a bit per instance saying
  whether it is drawn (its mesh ready, not left to the far field), and
  `load_instance` adds the instance's anchor back. Culling walks every
  instance and skips the unset ones; instances are addressed by their
  place in the frame everywhere. Per frame that is a bit per instance
  instead of 64 bytes.
- **Worlds stream by cell (N3 part 3, `editor/src/world_streaming.h`).** A
  world is a folder: `world.json` (`world::WorldIndex`: lighting, kits,
  the meshes its cells use, which cells have a file, the next id) and
  `cells/<x>_<z>.cell.json`, one per cell with entities (`world::save_cell`:
  sorted by id, so an unchanged cell saves the same text). Cells with any
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
  stand-ins for that, later). A cell joining or leaving the scene shifts
  the instances after it (cells go by z, then x), so the voxel world and
  the GPU store see a new layout and redo everything: while cells stream
  the voxel world keeps rebuilding. The next part keeps the layout stable
  as cells come and go. Meshes are imported when a world opens (the
  index lists them), not as cells stream. Unloading a cell is one
  `Scene::remove_many`, a pass over the scene's entities, and the unload
  look walks the loaded cells. Undoing an edit to a cell that was saved
  and unloaded since does nothing. A world cannot yet be saved to another folder, and its kits cannot
  be deleted while some cells are not loaded. Decals overlapping across a cell border draw in cell
  order, not the outliner's.

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
  or removed mesh counts as a change to the instances using it. More than
  256 changes in a frame (or an eighth of the instances), a backlog of over
  4,096, or any other removal rebuild every window, one rebuild at a time
  (another waits for it rather than restarting it, so constant changes
  still settle).
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
  across the light). Many changes at once (over max(256, an eighth))
  dirty everything. The copy is kept by index, so removing an instance
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
