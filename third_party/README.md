# Third-party libraries

Only permissive licences (MIT, BSD, zlib, Apache 2, Boost). Every library is
pinned to an exact version and listed here before it is used.

| Library | Version | Licence | Used by | Added in |
| --- | --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | 3.2.24 (`a8589a8`) | zlib | platform (windows, input) | N0 part 2 |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 (release archive, SHA-256 pinned) | MIT | ai (commands, MCP server), editor through ai | N0 part 3 |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | 1.92.9b (`b48d1af`) | MIT | editor (panels) | N0 part 3 |

Planned, from the plan's reuse table: Jolt Physics (MIT),
meshoptimizer (MIT), METIS (Apache 2), Slang (Apache 2), D3D12 Memory
Allocator (MIT), Tracy (BSD), miniaudio (MIT-0),
Steam Audio (Apache 2), Recast and Detour (zlib), .NET 10 runtime (MIT),
ufbx (MIT), cgltf (MIT), DirectStorage (Microsoft redistributable).
