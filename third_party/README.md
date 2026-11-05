# Third-party libraries

Only permissive licences (MIT, BSD, zlib, Apache 2, Boost). Every library is
pinned to an exact version and listed here before it is used.

| Library | Version | Licence | Used by | Added in |
| --- | --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | 3.2.24 (`a8589a8`) | zlib | platform (windows, input) | N0 part 2 |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 (release archive, SHA-256 pinned) | MIT | ai (commands, MCP server), editor through ai | N0 part 3 |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | 1.92.9b (`b48d1af`) | MIT | editor (panels) | N0 part 3 |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) (with `demo/clusterlod.h`) | 1.3 (`9e1f07b`) | MIT | geometry (clusters, simplification, cluster LOD) | N1 part 2 |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 (`360db1a`) | MIT | geometry (glTF 2.0 import) | N1 part 2 |
| [Slang](https://github.com/shader-slang/slang) compiler | 2026.19 (release binaries, SHA-256 pinned) | Apache 2.0 with LLVM exception | build time: compiles shaders | N1 part 1 |
| [DXC](https://github.com/microsoft/DirectXShaderCompiler) | 1.9.2609.5 (NuGet, SHA-256 pinned) | University of Illinois/NCSA (BSD-style) | build time: Slang's DXIL back end, Windows | N1 part 1 |

Platform runtimes (Microsoft redistributables, not open source; shipped next
to the executables like a system component, never compiled into engine code):

| Runtime | Version | Licence | Used by | Added in |
| --- | --- | --- | --- | --- |
| [DirectX 12 Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/) | 1.620.1 (NuGet, SHA-256 pinned) | Microsoft redistributable | rhi: D3D12 runtime and headers | N1 part 1 |
| [WARP](https://www.nuget.org/packages/Microsoft.Direct3D.WARP) | 1.0.22 (NuGet, SHA-256 pinned) | Microsoft redistributable | tests and `--software` only | N1 part 1 |

Planned, from the plan's reuse table: Jolt Physics (MIT),
D3D12 Memory
Allocator (MIT), Tracy (BSD), miniaudio (MIT-0),
Steam Audio (Apache 2), Recast and Detour (zlib), .NET 10 runtime (MIT),
ufbx (MIT), DirectStorage (Microsoft redistributable).

METIS from the plan is not needed: meshoptimizer's `meshopt_partitionClusters`
groups clusters for the LOD hierarchy.
