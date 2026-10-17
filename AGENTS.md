# Giant engine: rules for AI coding agents

This repository is the Giant engine, a fork of Godot 4.8 (development branch) built for
realistic open-world games about giants that grow to kilometre scale. Read `giant/README.md`
for what is different from upstream Godot.

## Ground rules

- Keep Giant code in its own modules (`modules/giant_*`) and in `giant/`. Touch upstream files
  only when a module cannot do the job, keep those edits small, and mark them with a
  `// Giant engine:` comment so upstream merges stay easy.
- Never edit `thirdparty/`.
- Every rendering feature must look clean with TAA off. The default is 4x MSAA plus SMAA.
  Do not add effects that rely on temporal accumulation in screen space.
- Every scene change made through the MCP tools must go through `EditorUndoRedoManager`.
- Match Godot's code style (tabs, the license header, `clang-format`).

## Build

Builds use SCons. Large world coordinates are always on (`precision=double`).

```
# Linux editor with tests
scons platform=linuxbsd target=editor precision=double tests=yes -j$(nproc)

# Windows editor (on Windows, or MinGW cross-compile)
scons platform=windows target=editor precision=double -j%NUMBER_OF_PROCESSORS%
```

The binary lands in `bin/`, for example `bin/godot.linuxbsd.editor.double.x86_64`.

## Test

```
# Engine unit tests (doctest)
bin/godot.linuxbsd.editor.double.x86_64 --headless --test

# MCP server end-to-end test: starts a headless editor and drives it over MCP
python3 giant/tests/mcp_e2e.py bin/godot.linuxbsd.editor.double.x86_64

# Giant prototype: renders it (xvfb and a software Vulkan driver on Linux) and saves screenshots
python3 giant/tests/demo_smoke.py bin/godot.linuxbsd.editor.double.x86_64 /tmp/giant-shots
```

Look at the screenshots after any visual change: they are the only way to see the result.

Run the narrowest test that covers your change first, then the full set before you finish.
A change is not done until its test passes.

## Where things live

| Path | What |
| --- | --- |
| `modules/giant_core/` | Giant nodes: `GiantBody` (size and time feel), `GiantJiggle3D`, `GiantFootsteps3D` |
| `giant/demo/` | The giant prototype project |
| `modules/giant_mcp/` | Built-in MCP server: lets Claude Code and the in-editor assistant drive the editor |
| `editor/editor_log.*` | Upstream file with a small Giant hook so the MCP server can read log output |
| `giant/README.md` | What the Giant engine adds and how to connect an AI to it |
| `giant/tests/` | Giant end-to-end tests and their test project |
