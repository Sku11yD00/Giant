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
- Part 3 (in progress): the AI command layer and MCP server are in, so
  Claude Code can drive the editor (see `docs/ai-control.md`). Next: editor
  panels and the C# script host.

The Windows editor download is attached to each CI run (`giant-editor-windows`).
It is a console app for now so its log is visible; run `giant_editor.exe` and
close the window to exit.

The earlier Godot-based work is archived on the branches
`giant/m0-foundation` and `giant/m1-giant-prototype`.

## Build

Needs CMake 3.25+, Ninja, Git, and a C++20 compiler (MSVC 2022, Clang 16+ or GCC 13+).
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
