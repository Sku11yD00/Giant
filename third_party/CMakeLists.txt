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
# A shallow clone of the release tag: the release tarball's download
# (github.com/.../releases/download) was failing with 504s on CI runners.
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
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
# stb_truetype and stb_rect_pack (public domain / MIT), as ImGui ships them:
# the game UI's fonts (engine/ui). Header only; one source file there
# compiles them, static. Its default font, Roboto Medium (Apache 2.0), comes
# from ImGui's misc/fonts too.
add_library(stb_truetype INTERFACE)
target_include_directories(stb_truetype SYSTEM INTERFACE ${imgui_SOURCE_DIR})
add_library(third_party::stb_truetype ALIAS stb_truetype)
set(GIANT_DEFAULT_FONT ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf CACHE INTERNAL "The game UI's default font")

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

# Jolt Physics: rigid bodies and collision for sim (MIT licence). Double
# precision, so bodies keep millimetres across a 30 km world. Built from its
# own CMake project with its extras (samples, tests, GPU compute, debug
# renderer, profiler, object streams) off and its own warning flags off.
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jolt
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  # Shallow clone of the release tag (commit e77f175595e64cb44218cc9d9d56fc365ad0e36a).
  GIT_TAG v5.6.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR Build
)
FetchContent_MakeAvailable(jolt)
set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Under the sanitizers Jolt is instrumented too: its threads hand bodies to
# ours through its own atomics, which a sanitizer must see.
giant_apply_sanitizers(Jolt)
if(GIANT_SANITIZE_THREAD AND NOT MSVC)
  # Jolt swaps its fences for acq_rel atomics under TSan, which it detects
  # with __has_feature or __SANITIZE_THREAD__. GCC's sanitizer headers
  # define __has_feature as 0, so a file including them first would see no
  # TSan (fences TSan cannot follow, and inline code differing from the
  # library's). Said once, for Jolt and everything using it.
  target_compile_definitions(Jolt PUBLIC JPH_TSAN_ENABLED=)
endif()
get_target_property(jolt_includes Jolt INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(Jolt PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${jolt_includes}")
add_library(third_party::jolt ALIAS Jolt)

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

# GitHub release downloads (Slang ships its compiler only as release assets)
# fail with 504s for minutes at a time on CI runners, so fetch them here with
# retries and a growing pause before handing the local file to FetchContent.
# They land in .downloads/ at the top of the tree, shared by every build
# folder (CI caches it).
function(giant_fetch_release name url sha256)
  get_filename_component(file ${url} NAME)
  set(path ${PROJECT_SOURCE_DIR}/.downloads/${file})
  set(have "")
  if(EXISTS ${path})
    file(SHA256 ${path} have)
  endif()
  foreach(pause 0 10 30 60 120)
    if(have STREQUAL sha256)
      break()
    endif()
    if(pause GREATER 0)
      message(STATUS "Download of ${file} failed; trying again in ${pause} s")
      execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${pause})
    endif()
    # No EXPECTED_HASH here: a failed download with one is a configure error.
    file(DOWNLOAD ${url} ${path} STATUS status)
    list(GET status 0 code)
    set(have "")
    if(code EQUAL 0)
      file(SHA256 ${path} have)
    endif()
  endforeach()
  if(NOT have STREQUAL sha256)
    file(REMOVE ${path})
    message(FATAL_ERROR "Could not download ${url}")
  endif()
  FetchContent_Declare(${name}
    URL ${path}
    URL_HASH SHA256=${sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
endfunction()

# Slang, the shader language (Apache 2.0 with LLVM exception), as the
# official prebuilt compiler. Shaders compile at build time; DXIL output
# needs Microsoft's DXC (NCSA licence), which Slang loads from its own folder.
if(GIANT_COMPILE_SHADERS)
  if(WIN32)
    giant_fetch_release(slang
      https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-windows-x86_64.zip
      fc922f214b5d115f663932e59e80e0f6b6cc6fae140979cf307da6459b6f8f33)
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
    giant_fetch_release(slang
      https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-linux-x86_64.tar.gz
      bd6cfc47b7353b2cffa36a866b1584749f76f4b2dbebf6dda26bc3fb40dc3c0e)
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

# stb_image: PNG, JPEG, TGA and BMP decoding for the texture cooker (public
# domain or MIT, you choose; v2.30). One header, pinned by commit and hash;
# the implementation is compiled here, outside our warning flags, with only
# the formats the cooker reads.
set(stb_commit f58f558c120e9b32c217290b80bad1a0729fbb2c)
set(stb_dir ${CMAKE_BINARY_DIR}/_deps/stb-${stb_commit})
file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/${stb_commit}/stb_image.h
     ${stb_dir}/stb_image.h
     EXPECTED_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 TLS_VERIFY ON)
set(stb_impl ${CMAKE_CURRENT_BINARY_DIR}/stb_image_impl.c)
file(WRITE ${stb_impl}.in
  "#define _CRT_SECURE_NO_WARNINGS\n#define STB_IMAGE_IMPLEMENTATION\n#define STBI_NO_STDIO\n#define STBI_NO_LINEAR\n#define STBI_NO_HDR\n"
  "#define STBI_ONLY_PNG\n#define STBI_ONLY_JPEG\n#define STBI_ONLY_TGA\n#define STBI_ONLY_BMP\n"
  "#define STBI_MAX_DIMENSIONS 16384\n#include \"stb_image.h\"\n")
configure_file(${stb_impl}.in ${stb_impl} COPYONLY)
add_library(stb_image_lib STATIC ${stb_impl})
target_include_directories(stb_image_lib SYSTEM PUBLIC ${stb_dir})
set_target_properties(stb_image_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(third_party::stb_image ALIAS stb_image_lib)

# bc7enc_rdo's block encoders and decoders (MIT or public domain, you
# choose): rgbcx (BC1, BC4, BC5) and bc7enc (BC7) for the texture cooker,
# bc7decomp to check its output in tests. Only these files, pinned by commit
# and hash (the repository's BC7E ISPC encoder is not used).
set(bc7enc_commit b9438627eef73a1157e84201b6fa6eb2ffd6d9f0)
set(bc7enc_dir ${CMAKE_BINARY_DIR}/_deps/bc7enc_rdo-${bc7enc_commit})
foreach(pair "rgbcx.h=111bd4c0211eca172bec9d757e2e597a47c4e4731bb122c9e8761dca85711bba"
             "rgbcx.cpp=059243ce8e8e3ec634bc1e3da3996115aaf36196a8d75a24d3dbb3c8bfe0ca98"
             "rgbcx_table4.h=5051cf4ce17cb1f9d011ef788b7c8d174f2270532da0514c9f394830bab72f36"
             "rgbcx_table4_small.h=d484802656fb7f2249674263bbb14399ab9ccb2a997ac0030e51b17a4bf8595f"
             "bc7enc.h=f40f4a956ed6a8845b7909e83831de09512424408b32ca29551bf49aee2fecf6"
             "bc7enc.cpp=b90444a091530a13c61fa6375583fcc9bc9ca47e474fd71f818876de68c1b581"
             "bc7decomp.h=cbba8778640e0021f55ca686b6bc19e6960032539084c70af698922b05c5b242"
             "bc7decomp.cpp=cfbe156926ebd96443b4407a9da1b5b7a424fdf989fedca3cf2fd0515e0c368a")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 bc7enc_file)
  list(GET pair 1 bc7enc_hash)
  file(DOWNLOAD https://raw.githubusercontent.com/richgel999/bc7enc_rdo/${bc7enc_commit}/${bc7enc_file}
       ${bc7enc_dir}/${bc7enc_file} EXPECTED_HASH SHA256=${bc7enc_hash} TLS_VERIFY ON)
endforeach()
add_library(bc7enc_lib STATIC ${bc7enc_dir}/rgbcx.cpp ${bc7enc_dir}/bc7enc.cpp ${bc7enc_dir}/bc7decomp.cpp)
target_include_directories(bc7enc_lib SYSTEM PUBLIC ${bc7enc_dir})
set_target_properties(bc7enc_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(third_party::bc7enc ALIAS bc7enc_lib)
