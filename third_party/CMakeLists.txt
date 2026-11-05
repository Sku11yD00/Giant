# Pinned third-party libraries. Every entry must also be listed in
# third_party/README.md with its licence. Our warning flags are set per
# target, so they never reach library sources; SYSTEM also keeps them off the
# libraries' headers when our code includes them.
include(FetchContent)

# SDL3: windows, input, gamepads (zlib licence).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  # Shallow clone of the release tag (commit a8589a84226a6202831a3d49ff4edda4acab9acd).
  GIT_TAG release-3.2.24
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(SDL3)

# nlohmann/json: JSON for the AI command layer and MCP server (MIT licence).
# Release archive with only the headers and CMake files.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

# Dear ImGui, docking branch: editor panels (MIT licence). Only the core
# files: the editor brings its own platform and renderer glue.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  # Shallow clone of the release tag (commit b48d1afbe8ee8b238e2961dc363a949dd7304e23).
  GIT_TAG v1.92.9b-docking
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)
add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(imgui_lib SYSTEM PUBLIC ${imgui_SOURCE_DIR})
# 32-bit indices: the UI layer hands index buffers to the GPU as they are.
target_compile_definitions(imgui_lib PUBLIC "ImDrawIdx=unsigned int")
set_target_properties(imgui_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(third_party::imgui ALIAS imgui_lib)

# meshoptimizer: cluster building and simplification for virtual geometry
# (MIT licence), plus its clusterlod.h cluster-LOD builder from demo/, which
# the author publishes for engines to use as is. Built here from its sources
# (SOURCE_SUBDIR skips its own CMake project).
FetchContent_Declare(meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  # Shallow clone of the release tag (commit 9e1f07b159d3cb777f1c67ed31fc11fd117986f4).
  GIT_TAG v1.3
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR no-cmake
)
FetchContent_MakeAvailable(meshoptimizer)
file(GLOB meshoptimizer_sources ${meshoptimizer_SOURCE_DIR}/src/*.cpp)
set(clusterlod_impl ${CMAKE_CURRENT_BINARY_DIR}/clusterlod_impl.cpp)
file(WRITE ${clusterlod_impl}.in
  "#include \"meshoptimizer.h\"\n#define CLUSTERLOD_IMPLEMENTATION\n#include \"clusterlod.h\"\n")
configure_file(${clusterlod_impl}.in ${clusterlod_impl} COPYONLY)
add_library(meshoptimizer_lib STATIC ${meshoptimizer_sources} ${clusterlod_impl})
target_include_directories(meshoptimizer_lib SYSTEM PUBLIC
  ${meshoptimizer_SOURCE_DIR}/src ${meshoptimizer_SOURCE_DIR}/demo)
set_target_properties(meshoptimizer_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(third_party::meshoptimizer ALIAS meshoptimizer_lib)

# cgltf: glTF 2.0 reader for mesh import (MIT licence), one header. The
# implementation is compiled here, outside our warning flags.
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  # Shallow clone of the release tag (commit 360db1a95480fe102ae9c69b27c5d101167ff5ba).
  GIT_TAG v1.15
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR no-cmake
)
FetchContent_MakeAvailable(cgltf)
set(cgltf_impl ${CMAKE_CURRENT_BINARY_DIR}/cgltf_impl.c)
file(WRITE ${cgltf_impl}.in "#define CGLTF_IMPLEMENTATION\n#include \"cgltf.h\"\n")
configure_file(${cgltf_impl}.in ${cgltf_impl} COPYONLY)
add_library(cgltf_lib STATIC ${cgltf_impl})
target_include_directories(cgltf_lib SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
set_target_properties(cgltf_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(third_party::cgltf ALIAS cgltf_lib)

# --- GPU toolchain -----------------------------------------------------------
# NuGet packages are zip files; DOWNLOAD_NAME lets CMake unpack them.

if(WIN32)
  # DirectX 12 Agility SDK: the D3D12 runtime the engine ships next to its
  # executables (Microsoft redistributable), so shader model 6.6, bindless
  # resources and mesh shaders work on any Windows 10/11 PC, not only on the
  # newest OS builds. Also brings newer d3d12.h headers.
  FetchContent_Declare(d3d12_agility
    URL https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.620.1/microsoft.direct3d.d3d12.1.620.1.nupkg
    URL_HASH SHA256=a8a494c3e5f687d9be0a465ec8248da70781ab689d888db8806b600422722be6
    DOWNLOAD_NAME d3d12_agility.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  # WARP, the software GPU, in the version that matches the Agility SDK.
  # Tests and --software runs use it; it is not part of the editor download.
  FetchContent_Declare(d3d12_warp
    URL https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/1.0.22/microsoft.direct3d.warp.1.0.22.nupkg
    URL_HASH SHA256=afc058f4d1066edc4231a38f3ab3592b1930f7287d4ac700e6a056e0874bb0be
    DOWNLOAD_NAME d3d12_warp.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(d3d12_agility d3d12_warp)
  # Must match the Agility SDK version above (its minor version number).
  set(GIANT_D3D12_SDK_VERSION 620 CACHE INTERNAL "")
  set(GIANT_D3D12_AGILITY_DIR ${d3d12_agility_SOURCE_DIR} CACHE INTERNAL "")
  set(GIANT_D3D12_WARP_DIR ${d3d12_warp_SOURCE_DIR} CACHE INTERNAL "")
  add_library(d3d12_agility_headers INTERFACE)
  target_include_directories(d3d12_agility_headers SYSTEM INTERFACE
    ${d3d12_agility_SOURCE_DIR}/build/native/include)
  add_library(third_party::d3d12_headers ALIAS d3d12_agility_headers)
endif()

# Slang, the shader language (Apache 2.0 with LLVM exception), as the
# official prebuilt compiler. Shaders compile at build time; DXIL output
# needs Microsoft's DXC (NCSA licence), which Slang loads from its own folder.
if(GIANT_COMPILE_SHADERS)
  if(WIN32)
    FetchContent_Declare(slang
      URL https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-windows-x86_64.zip
      URL_HASH SHA256=fc922f214b5d115f663932e59e80e0f6b6cc6fae140979cf307da6459b6f8f33
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_Declare(dxc
      URL https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.dxc/1.9.2609.5/microsoft.direct3d.dxc.1.9.2609.5.nupkg
      URL_HASH SHA256=73cfa082245918c2cc1ec6740250eb7377e83a757b3b3a3e16e0d71535bfbfc3
      DOWNLOAD_NAME dxc.zip
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(slang dxc)
    foreach(dll dxcompiler.dll dxil.dll)
      file(COPY ${dxc_SOURCE_DIR}/build/native/bin/x64/${dll} DESTINATION ${slang_SOURCE_DIR}/bin)
    endforeach()
    set(GIANT_SLANGC ${slang_SOURCE_DIR}/bin/slangc.exe CACHE INTERNAL "")
  else()
    FetchContent_Declare(slang
      URL https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-linux-x86_64.tar.gz
      URL_HASH SHA256=bd6cfc47b7353b2cffa36a866b1584749f76f4b2dbebf6dda26bc3fb40dc3c0e
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(slang)
    set(GIANT_SLANGC ${slang_SOURCE_DIR}/bin/slangc CACHE INTERNAL "")
  endif()
endif()

# SMAA's precomputed area and search textures (MIT licence), as C arrays.
# The shader is our own port (engine/render/shaders/smaa.slang).
# Only the two headers are downloaded, pinned by commit and hash.
set(smaa_commit 71c806a838bdd7d517df19192a20f0c61b3ca29d)
set(smaa_dir ${CMAKE_BINARY_DIR}/_deps/smaa-${smaa_commit})
foreach(pair "AreaTex.h=1933bf43ce86ba71f7ed5f0fa94b148610c634dc3395b9bc67a817d51ee73af2"
             "SearchTex.h=1fac315ab5f87b60083c9b8387af17f056204d3d1bf21500f2acc5b3e8c1a37a")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 smaa_file)
  list(GET pair 1 smaa_hash)
  # Always checked against the hash: a file that already matches is kept
  # (no download, works offline); a bad or partial one is fetched again.
  file(DOWNLOAD https://raw.githubusercontent.com/iryoku/smaa/${smaa_commit}/Textures/${smaa_file}
       ${smaa_dir}/${smaa_file} EXPECTED_HASH SHA256=${smaa_hash} TLS_VERIFY ON)
endforeach()
add_library(smaa_textures INTERFACE)
target_include_directories(smaa_textures SYSTEM INTERFACE ${smaa_dir})
add_library(third_party::smaa ALIAS smaa_textures)
