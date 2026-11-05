# Architecture

Five layers. Each layer only uses the layers below it.

```
game        C# scripts, levels, materials, scatter graphs, weather presets
editor      panels, play-in-editor, undo; uses everything below
ai          command layer, MCP server, rule checks
sim world   giants, destruction, NPCs, animation | cells, streaming, landscape, scatter, weather
render      virtual geometry, voxel world, GI, materials, hair, clouds
rhi geometry thin graphics layer (D3D12 first) | meshes, import, cluster LOD (CPU only)
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
| render | core, rhi, geometry |
| world | core |
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
  constants, instance lists and UI geometry.
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
  coarsest level of every mesh is always loaded. Geometry comes from a CPU
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
  views. Shading renders into its own sRGB image first. Specular: in the
  shading pass, the change in normal across each 2x2 pixel quad (same
  instance only) widens the GGX roughness (Kaplanyan-Hoffman style, capped),
  so highlights on tight curves stay stable instead of flickering.
- **CPU path.** `gpu_culling = false` chooses the cut on the CPU (frustum
  only) and feeds the same raster and shading passes; tests use it for
  exact counts.

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

## Frame budget (1440p, 60 fps, reference GPU, High preset)

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
