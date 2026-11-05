# Giant Engine

A custom game engine for one kind of game: a huge, photoreal open world that a
growing giantess can wreck, cities and forests included, with a sharp image
(no TAA smear, no motion blur) and an AI that can do every job in the editor.

The full design is in the plan doc (v2 tab, "Custom engine plan"). The short
version:

| Part | Choice |
| --- | --- |
| Engine core | C++20, strict house subset (see `docs/coding-rules.md`) |
| Game scripts | C# on .NET 10 (from milestone N0 part 3) |
| Shaders | Slang |
| Graphics | Direct3D 12 first, behind a thin layer so Vulkan can follow |
| First platform | Windows; Linux builds the core and runs all tests |

## Status

Milestone **N0 Foundation**:

- Part 1 (done): layout, build system, rule book, core library (logging,
  asserts, double-precision world math, job system), architecture checker, CI.
- Part 2 (done): SDL3 window, the graphics layer with a Direct3D 12 backend,
  and the editor app that opens a window and renders every frame.
- Part 3 (done): the AI command layer and MCP server (Claude Code can
  drive the editor and take screenshots, see `docs/ai-control.md`), editor
  panels (Dear ImGui, docked), and C# scripting on .NET 10 with reload
  (see `docs/scripting.md`).

Milestone **N1 Virtual geometry** (in progress):

- Part 1 (done): the real graphics layer (bindless, shader model 6.6,
  frames in flight, Slang shaders, Agility SDK), the scene (entities,
  lighting, save and load), a lit forward renderer and a 3D viewport with a
  fly camera (hold the right mouse button, then W A S D). Claude can build,
  light and photograph a scene over MCP alone, which closes N0's gate.
- Part 2 (done): mesh import (glTF 2.0 and GLB) and cluster LOD, the
  Nanite method: every mesh is cut into clusters of up to 128 triangles,
  simplified level by level into a hierarchy, and drawn each frame at the
  detail that keeps the error under a pixel. Try it with the Scene panel's
  "+ Mesh" field, and the Engine panel's View menu (LOD levels) to see it work.
- Part 3 (done): the detail choice moved to the GPU, with frustum and
  two-pass occlusion culling (hidden geometry is skipped), mesh shaders
  (with a vertex shader fallback) and a visibility buffer that shades each
  pixel once. The Engine panel has switches for each and shows what was hidden.
- Part 4 (done): cluster streaming. Geometry lives in a fixed pool of GPU
  memory that holds only the detail the view needs: closer detail loads over
  a few frames as the camera moves (coarser clusters stand in until then),
  and detail nobody has looked at for a while makes room. The Engine panel
  shows how full the pool is.
- Part 5 (done): kits and decals. A kit is a reusable group of pieces (a
  modular wall, a house, a tree with its rocks) placed any number of times;
  editing the kit updates every placement, and every placement shares its
  meshes and materials, so a town of a thousand houses is drawn in the same
  few passes as one. Decals stamp colour and roughness onto whatever is
  under them (mud, scorch marks, cracked ground from a giant's footfall),
  with five shapes for now. Try "+ Decal" and "Selected to kit" in the
  Scene panel.
- Part 6 (done, N1 complete): sharp anti-aliasing and the N1 gate. Jagged
  edges are smoothed from the finished image alone (SMAA), so nothing smears
  or ghosts when the camera moves, and shiny highlights are widened where a
  surface curves faster than a pixel can show, so they do not sparkle. Both
  have switches in the Engine panel. The gate scene, a kit-built town and
  10,000 trees, still draws in the same few passes; `tools/n1_gate.py` flies
  through it and reports whether your GPU holds 60 fps:

      python tools/n1_gate.py giant_editor.exe

Milestone **N2, voxel world and GI** (in progress; gate: a 20 km view that stays sharp while moving, GI and reflections under 4 ms, no hitches):

- Part 1 (done): stutter-free uploads and GPU timing. Buffers and textures
  made inside a frame (streaming's own tables, meshes added mid-frame) no
  longer stop the CPU to wait for the GPU: their data is copied on the GPU
  ahead of the frame that uses them. (The editor's mesh import still runs
  between frames and waits; it moves into the frame with N3.) Streaming
  spreads even the coarsest levels of many newly added meshes over frames
  within its per-frame budget, so adding a forest at once never makes one
  frame upload all of it. The GPU now times its own passes (streaming,
  culling, raster, depth pyramid, shading, anti-aliasing, UI); the Engine panel and
  `get_engine_info` show the times.

The Windows editor download is attached to each CI run (`giant-editor-windows`).
It is a console app for now so its log is visible; run `giant_editor.exe` and
close the window to exit. Keep the `D3D12` folder next to the exe. The N1
performance gate comes with it: in that folder, run
`python n1_gate.py giant_editor.exe` (Python 3). C# scripts need the .NET 10 runtime
(https://dotnet.microsoft.com/download/dotnet/10.0); the rest works without it.

The earlier Godot-based work is archived on the branches
`giant/m0-foundation` and `giant/m1-giant-prototype`.

## Build

Needs CMake 3.25+, Ninja, Git, a C++20 compiler (MSVC 2022, Clang 16+ or GCC 13+),
and the .NET 10 SDK for C# scripting (optional; without it scripting is skipped).
On Linux, SDL3 also needs the X11 headers (`sudo apt install libx11-dev libxext-dev`).

```sh
cmake --preset dev          # debug build with sanitizers (Linux)
cmake --build --preset dev
ctest --preset dev
```

On Windows, open a "x64 Native Tools" prompt and use the `msvc` preset, or open
the folder in Visual Studio.

`python3 tools/check_architecture.py` checks that every module only includes
the modules it is allowed to (CI runs it on every push).

## Layout

| Folder | Holds |
| --- | --- |
| `engine/core` | Memory, threads, jobs, math (double precision), logging, reflection, file formats |
| `engine/platform` | Windows, input and files, on SDL3 |
| `engine/rhi` | Thin render layer, D3D12 backend first |
| `engine/render` | Virtual geometry, voxel world, GI, shadows, materials, hair, sky and clouds |
| `engine/world` | Cells, streaming, landscape, scatter, weather simulation |
| `engine/sim` | Physics glue (Jolt), destruction, giants, animation, NPC simulation, navigation |
| `engine/script` | .NET host, C# game framework |
| `engine/ai` | Command layer, MCP server, rule checks |
| `editor` | Editor app |
| `game` | The game project (C# scripts, content) |
| `shaders` | Slang shader library and material types |
| `third_party` | Pinned open-source libraries |
| `tests` | Shared test harness; unit tests live next to each module |
| `docs` | Architecture and rules |

AI agents: read `AGENTS.md` first.
