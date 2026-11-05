# Architecture

Five layers. Each layer only uses the layers below it.

```
game        C# scripts, levels, materials, scatter graphs, weather presets
editor      panels, play-in-editor, undo; uses everything below
ai          command layer, MCP server, rule checks
sim world   giants, destruction, NPCs, animation | cells, streaming, landscape, scatter, weather
render      virtual geometry, voxel world, GI, materials, hair, clouds
rhi         thin graphics layer (D3D12 first)
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
| render | core, rhi |
| world | core |
| sim | core, world |
| script | core, world, sim |
| ai | core, platform, rhi, render, world, sim, script |
| editor | everything |

`rhi` never includes `platform`: a swap chain takes the native window handle
as plain data, so the graphics layer works headless and in tests.

`render` reads world state through plain data handed to it each frame (cells,
instances, weather values, voxel edits); it never includes `world` or `sim`.
This keeps the renderer replaceable.

## Graphics layer plan

N0's `rhi` is deliberately small: one device, swap chains, clear, present,
and an offscreen read-back used to prove the device renders. N1 grows it into
the real layer before any rendering code depends on its shape:

- Frames in flight move onto the device: a ring of per-frame command
  allocators per recording thread, so several swap chains (editor viewports)
  and headless frames work. Swap chains only acquire and present.
- Recording moves into command lists; clears become command-list calls.
- Buffers, textures, pipelines and bindless descriptors (shader model 6.6).
- Swap chains track their device in debug builds so destroying the device
  first is caught.

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
