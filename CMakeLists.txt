cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
project(GiantEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Static C runtime on Windows: the editor download runs without installing
# the Visual C++ redistributable. Applies to third-party code too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(GIANT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(GIANT_SANITIZE_THREAD "Build with ThreadSanitizer" OFF)
option(GIANT_BUILD_TESTS "Build unit tests" ON)
option(GIANT_REQUIRE_VALIDATION "Tests fail if the D3D12 validation layer is unavailable" OFF)
# Shaders compile with the prebuilt Slang compiler, downloaded at configure
# time. Windows needs them (the GPU runs them); elsewhere they are only
# checked, on CI. Without them, shader blobs are empty and only the null
# backend works.
if(WIN32)
  set(giant_shaders_default ON)
else()
  set(giant_shaders_default OFF)
endif()
option(GIANT_COMPILE_SHADERS "Compile shaders with Slang (needed for the D3D12 backend)" ${giant_shaders_default})
option(GIANT_REQUIRE_DOTNET "Fail the configure if the .NET SDK is missing (CI)" OFF)

# The .NET SDK builds the C# API and scripts. Without it the engine still
# builds, but C# scripting is unavailable and its tests are skipped.
find_program(GIANT_DOTNET dotnet)
if(NOT GIANT_DOTNET)
  if(GIANT_REQUIRE_DOTNET)
    message(FATAL_ERROR "GIANT_REQUIRE_DOTNET is on but the .NET SDK (dotnet) was not found")
  endif()
  message(WARNING "The .NET 10 SDK was not found: C# scripting will not be built")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GiantModule)
include(GiantShaders)

if(GIANT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

add_subdirectory(third_party)
add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/rhi)
add_subdirectory(engine/anim)
add_subdirectory(engine/geometry)
add_subdirectory(engine/voxel)
add_subdirectory(engine/world)
add_subdirectory(engine/sim)
add_subdirectory(engine/gameplay)
add_subdirectory(engine/render)
add_subdirectory(engine/script)
add_subdirectory(engine/ai)
add_subdirectory(editor)
