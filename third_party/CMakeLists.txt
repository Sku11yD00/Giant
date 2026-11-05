# Pinned third-party libraries. Every entry must also be listed in
# third_party/README.md with its licence. Libraries are built as SYSTEM so
# our warnings-as-errors never apply to their code or headers.
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
  GIT_TAG a8589a84226a6202831a3d49ff4edda4acab9acd # release-3.2.24
  GIT_SHALLOW FALSE
  SYSTEM
)
FetchContent_MakeAvailable(SDL3)
