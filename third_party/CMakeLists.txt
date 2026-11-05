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
